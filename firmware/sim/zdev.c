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
 *   build/zdev [--mnemonic "..."] [--passphrase P] [--network test|main] [--yes]
 *              [--link /tmp/zdev]
 *
 * Prints the PTY path (and makes --link a symlink to it); point the
 * companion's --port there.
 *
 * --network is the board's Settings > Network (default test, or the
 * ZDEV_NETWORK environment variable): like a board, zdev answers only for
 * that network and refuses the other with 0x0528. SIGUSR1 switches it to
 * Mainnet and SIGUSR2 to Testnet while it runs, as the user would on the
 * board, keeping the pairing and everything stored.
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
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

/* Settings > Network: 0 mainnet, 1 testnet (ZS_NET_*). Written by the signal
 * handlers, read by protocol.c through ui_zcash_network(). A signal raised
 * before a request is written is handled before zdev reads that request: the
 * handler runs on the way out of the poll() or read() that would see it. */
static volatile sig_atomic_t board_network = 1;
static volatile sig_atomic_t network_switched = 0;

static void on_mainnet(int sig) { (void)sig; board_network = 0; network_switched = 1; }
static void on_testnet(int sig) { (void)sig; board_network = 1; network_switched = 1; }

static const char *network_title(int network) { return network == 0 ? "Mainnet" : "Testnet"; }

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
    /* As the real driver: a write larger than the TX ring buffer is not
     * queued at all. Without this the host never saw the bug that ate every
     * Zcash reply on the first board. */
    if (size > PROTOCOL_USB_TX_BUF) {
        return 0;
    }
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
uint8_t ui_zcash_network(void) { return (uint8_t)board_network; }

void ui_notify_wrong_network(uint8_t asked)
{
    printf("\n[device] Wrong network: the app asked for %s; this device is set to %s.\n",
           network_title(asked), network_title(board_network));
}
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

void ui_request_show_address(const char *ua, const char *t_address, uint32_t account,
                             uint8_t network)
{
    printf("\n[device] Address check, account %u, %s:\n  %s\n  (t-address %s)\n",
           (unsigned)account, network == ZS_NET_MAIN ? "mainnet" : "testnet", ua, t_address);
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
    fprintf(stderr, "usage: zdev [--mnemonic WORDS] [--passphrase P] [--network test|main] [--yes] "
                    "[--link PATH]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *mnemonic = DEFAULT_MNEMONIC, *passphrase = NULL, *link_path = NULL;
    const char *network = getenv("ZDEV_NETWORK");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mnemonic") && i + 1 < argc) mnemonic = argv[++i];
        else if (!strcmp(argv[i], "--passphrase") && i + 1 < argc) passphrase = argv[++i];
        else if (!strcmp(argv[i], "--network") && i + 1 < argc) network = argv[++i];
        else if (!strcmp(argv[i], "--link") && i + 1 < argc) link_path = argv[++i];
        else if (!strcmp(argv[i], "--yes")) auto_yes = true;
        else usage();
    }
    if (network && *network) {
        if (strcmp(network, "main") && strcmp(network, "test")) usage();
        board_network = strcmp(network, "main") == 0 ? 0 : 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* No SA_RESTART: poll() returns EINTR, and the loop reports the switch. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_mainnet;
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = on_testnet;
    sigaction(SIGUSR2, &sa, NULL);

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
    printf("zdev: Settings > Network: %s\n", network_title(board_network));

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
        if (network_switched) {
            network_switched = 0;
            printf("[device] Settings > Network: %s\n", network_title(board_network));
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
