/* zdev: the device's protocol endpoint on a PC, over a pseudo-terminal.
 *
 * Development only. It runs the firmware's own protocol.c, session.c,
 * zcash-account.c, zcash-sign.c (both signer backends, BOTH mode) and
 * zcash-review.c, so a companion (zecz) can pair, export the account and
 * have a PCZT checked, reviewed and signed exactly as a board would, with no
 * board. What it does not have is the board: the screen is this terminal,
 * the buttons are y/n on stdin, and the seed comes from --mnemonic through
 * the sim's fake wallet rather than the encrypted vault. Never use it with a
 * seed that holds real funds.
 *
 *   build/zdev [--mnemonic "..."] [--passphrase P] [--yes] [--link /tmp/zdev]
 *
 * Prints the PTY path (and makes --link a symlink to it); point the
 * companion's --port there.
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fake_nvs.h"
#include "fake_wallet.h"
#include "leek-wallet.h"
#include "pin.h"
#include "protocol.h"
#include "session.h"
#include "ui.h"
#include "zcash-review.h"
#include "zcash-sign.h"

void protocol__pump_for_test(void);
void protocol_set_rx_enabled(bool enabled);

static const char *DEFAULT_MNEMONIC =
    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon "
    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon "
    "abandon art";

static int  pty_master = -1;
static bool auto_yes = false;

/* ------------------------------------------------------------ the port */

#define RXQ_CAP 65536
static uint8_t rxq[RXQ_CAP];
static size_t  rxq_len;

esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *config)
{
    (void)config;
    return ESP_OK;
}

int usb_serial_jtag_read_bytes(void *buf, size_t size, TickType_t ticks)
{
    (void)ticks;
    size_t n = rxq_len < size ? rxq_len : size;
    memcpy(buf, rxq, n);
    memmove(rxq, rxq + n, rxq_len - n);
    rxq_len -= n;
    return (int)n;
}

int usb_serial_jtag_write_bytes(const void *src, size_t size, TickType_t ticks)
{
    (void)ticks;
    const uint8_t *p = src;
    size_t left = size;
    while (left) {
        ssize_t w = write(pty_master, p, left);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return 0;
        }
        p += w;
        left -= (size_t)w;
    }
    return (int)size;
}

esp_err_t usb_serial_jtag_wait_tx_done(TickType_t ticks)
{
    (void)ticks;
    return ESP_OK;
}

/* No scheduler: ticks move only when protocol.c waits, as in fake_usb.c. */
static TickType_t ticks_now;
TickType_t xTaskGetTickCount(void) { return ticks_now; }
void vTaskDelay(TickType_t ticks) { ticks_now += ticks ? ticks : 1; }
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                       uint32_t prio, TaskHandle_t *out)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio;
    if (out) *out = NULL;
    return pdPASS;
}

/* ------------------------------------------------------- screen/buttons */

static bool ask(const char *question)
{
    if (auto_yes) {
        printf("%s y (--yes)\n", question);
        return true;
    }
    printf("%s [y/N] ", question);
    fflush(stdout);
    char line[16];
    if (!fgets(line, sizeof line, stdin)) return false;
    return line[0] == 'y' || line[0] == 'Y';
}

static bool session_confirm_pending;
static bool question_pending;
static SignOutcome outcome = SIGN_PENDING;

uint32_t ui_hd_account(void) { return 0; }
bool ui_user_is_answering(void) { return question_pending; }
void ui_request_unlock(void) { printf("[device] unlock requested (already unlocked)\n"); }
void ui_request_lock(void) { printf("[device] lock requested\n"); }

void ui_request_session_confirm(void)
{
    /* Answered from the main loop, after the reveal reply has gone out:
     * the host only shows its digits once it has that reply. */
    printf("\n[device] Pairing request. Passkey on this device: %s\n", session_passkey());
    session_confirm_pending = true;
}

void ui_request_passphrase_confirm(uint32_t fingerprint)
{
    printf("\n[device] The app set a passphrase. Seed fingerprint XFP %08X\n", fingerprint);
    question_pending = true;
    outcome = SIGN_PENDING;
}

void ui_request_zsign_review(const zs_summary *s, uint8_t network)
{
    int n = zreview_page_count(s, network);
    printf("\n[device] Transaction review (%s backend), %d pages:\n",
           zcash_sign_backend_name(zcash_sign_backend()), n);
    for (int i = 0; i < n; i++) {
        ZReviewPage p;
        zreview_render(s, network, i, &p);
        printf("  +---------------------+\n");
        for (int r = 0; r < ZREVIEW_ROWS; r++) printf("  |%-21s|\n", p.row[r]);
    }
    printf("  +---------------------+\n");
    question_pending = true;
    outcome = SIGN_PENDING;
}

/* Polled by protocol.c's wait_for_user(); the first poll asks. */
SignOutcome ui_sign_outcome(void)
{
    if (question_pending && outcome == SIGN_PENDING) {
        outcome = ask("[device] Approve?") ? SIGN_APPROVED : SIGN_REJECTED;
        question_pending = false;
    }
    return outcome;
}

void ui_sign_clear(void) { outcome = SIGN_PENDING; }
void ui_sign_expire(void) { question_pending = false; }

/* ---------------------------------------------------------------- main */

static void usage(void)
{
    fprintf(stderr, "usage: zdev [--mnemonic WORDS] [--passphrase P] [--yes] [--link PATH]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *mnemonic = DEFAULT_MNEMONIC, *passphrase = NULL, *link_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mnemonic") && i + 1 < argc) mnemonic = argv[++i];
        else if (!strcmp(argv[i], "--passphrase") && i + 1 < argc) passphrase = argv[++i];
        else if (!strcmp(argv[i], "--link") && i + 1 < argc) link_path = argv[++i];
        else if (!strcmp(argv[i], "--yes")) auto_yes = true;
        else usage();
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* An unlocked device holding the seed, as after PIN entry on a board. */
    fake_nvs_reset();
    pin_init();
    if (!pin_set("1234") || !pin_verify("1234")) {
        fprintf(stderr, "zdev: PIN setup failed\n");
        return 1;
    }
    wallet_unlock("1234", 4);
    if (!fake_wallet_preload(mnemonic)) {
        fprintf(stderr, "zdev: not a valid BIP39 mnemonic\n");
        return 1;
    }
    if (passphrase && wallet_set_passphrase(passphrase, strlen(passphrase)) != WALLET_OK) {
        fprintf(stderr, "zdev: passphrase refused\n");
        return 1;
    }

    int slave = -1;
    char name[256];
    struct termios raw;
    memset(&raw, 0, sizeof raw);
    cfmakeraw(&raw);
    if (openpty(&pty_master, &slave, name, &raw, NULL) != 0) {
        perror("openpty");
        return 1;
    }
    /* `slave` stays open here so the master never sees a hangup when a
     * companion disconnects; the next one finds the device as it was. */
    if (link_path) {
        unlink(link_path);
        if (symlink(name, link_path) != 0) perror("symlink");
    }
    printf("zdev: LeekWallet Zcash dev device on %s%s%s\n", name, link_path ? " -> " : "",
           link_path ? link_path : "");
    printf("zdev: DEV ONLY. Fake vault; never use a seed holding real funds.\n");

    protocol_set_rx_enabled(true);
    for (;;) {
        struct pollfd pfd = { .fd = pty_master, .events = POLLIN };
        int pr = poll(&pfd, 1, 1000);
        if (pr < 0 && errno != EINTR) {
            perror("poll");
            return 1;
        }
        if (pr > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(pty_master, rxq + rxq_len, RXQ_CAP - rxq_len);
            if (n > 0) rxq_len += (size_t)n;
            protocol__pump_for_test();
        }
        if (session_confirm_pending) {
            session_confirm_pending = false;
            if (ask("[device] Does the companion show the same passkey?")) {
                session_confirm();
                printf("[device] Paired.\n");
            } else {
                session_reset();
                printf("[device] Pairing refused.\n");
            }
        }
    }
}
