/**
 * LeekWallet UI Framework
 * Screen state machine and rendering
 */

#include "ui.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "oled.h"
#include "button.h"
#include "pin.h"
#include "leek-wallet.h"
#include "mnemonic-entry.h"
#include "bip39.h"
#include "memzero.h"
#include "entropy.h"
#include "session.h"
#include "text-entry.h"
#include "device-wipe.h"
#include "transport.h"
#include "protocol.h"
#include "ble.h"
#include "ble-name.h"
#include "esp_timer.h"
#include "qr-out.h"
#include "splash.h"
#include "version.h"
#include "airgap.h"
#if LEEK_HAS_ZCASH
#include "zcash-sign.h"
#endif
#include "board.h"
#include "hints.h"
#if LEEK_HAS_ZCASH
#include "zcash-account.h"
#include "zcash-fp.h"
#include "zcash-meta.h"
#include "zcash-receive.h"
#include "zcash-review.h"
#endif
#if LEEK_HAS_SDCARD && LEEK_HAS_ZCASH
#include "sd-seed.h"
#include "sd-sign.h"
#define UI_HAS_SD_SEED 1
#else
#define UI_HAS_SD_SEED 0
#endif
#include "camera.h"
#include "viewfinder.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

/* For WiFi/BLE/USB testing - conditionally included */
#ifdef CONFIG_ESP_WIFI_ENABLED
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#endif

#ifdef CONFIG_BT_NIMBLE_ENABLED
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#elif defined(CONFIG_BT_ENABLED)
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#endif

#ifdef CONFIG_TINYUSB_ENABLED
#include "tinyusb.h"
#include "tusb_hid.h"
#endif

static const char *TAG = "ui";

/* Screen registry */
static const screen_t *screens[SCREEN_COUNT] = {NULL};
static screen_id_t current_screen = SCREEN_BOOT;
static bool needs_render = true;

/* ----------------------------------------------------------- boot splash
 *
 * The boot screen is the looping splash (splash.h): logo, text, a blinking
 * HINT_PRESS_ANY, again, until the user asks for the PIN. The unlock screen
 * falls back to it after UI_PIN_IDLE_SPLASH_US with no input.
 *
 * What dismisses it: on the knob kit only the press (OK); a turn or Cancel
 * jumps to the prompt, which says "Push the knob". On the four-button boards
 * any key, as "Press any key" says. Either way the press that wakes a
 * blanked panel is spent waking it. A variable rather than the macro so the
 * host suite, built for the four-button board, can drive both. */
static bool splash_ok_only = LEEK_HAS_ENCODER;
static splash_loop_t boot_splash;
static splash_view_t boot_splash_shown;   /* what the panel has now */

/* The prompt shares the wide layout with the logo: 15 columns beside it. */
_Static_assert(sizeof(HINT_PRESS_ANY) - 1 <= 15, "the splash prompt does not fit beside the logo");

/* The PIN unlock screen, left alone this long with a partial PIN or none,
 * goes back to the splash. Leaving through ui_set_screen() runs its exit hook
 * (forget_pin_entry), which zeroes the typed digits; nothing is submitted, so
 * the attempt counter in pin.c is neither spent nor reset. */
#define UI_PIN_IDLE_SPLASH_US (30LL * 1000 * 1000)
static int64_t pin_idle_since_us;

static int64_t ui_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* A view nothing can equal, so the first check after a (re)start repaints. */
static void boot_splash_restart(void)
{
    splash_loop_start(&boot_splash, ui_now_ms());
    boot_splash_shown = (splash_view_t){ .blank = false, .frame = ~0u, .prompt = false };
}

/* Forward declarations for built-in screens */
static void screen_boot_enter(void);
static void screen_boot_render(void);
static void screen_boot_on_button(button_id_t btn);

static void screen_pin_setup_enter(void);
static void screen_pin_setup_render(void);
static void screen_pin_setup_on_button(button_id_t btn);

static void screen_pin_unlock_enter(void);
static void screen_pin_unlock_render(void);
static void screen_pin_unlock_on_button(button_id_t btn);

static void screen_main_menu_enter(void);
static void screen_main_menu_render(void);
static void screen_main_menu_on_button(button_id_t btn);

static void wallet_info_refresh(void);
static void screen_wallet_info_enter(void);
static void screen_wallet_info_render(void);
static void screen_wallet_info_on_button(button_id_t btn);

static void screen_wallet_create_enter(void);
static void screen_wallet_create_render(void);
static void screen_wallet_create_on_button(button_id_t btn);

static void screen_wallet_select_enter(void);
static void screen_wallet_select_render(void);
static void screen_wallet_select_on_button(button_id_t btn);

static void screen_mnemonic_display_enter(void);
static void screen_mnemonic_display_render(void);
static void screen_mnemonic_display_on_button(button_id_t btn);

static void screen_mnemonic_entry_enter(void);
static void screen_mnemonic_entry_render(void);
static void screen_mnemonic_entry_on_button(button_id_t btn);

static void screen_settings_enter(void);
static void screen_settings_render(void);
static void screen_settings_on_button(button_id_t btn);

static void screen_entropy_enter(void);
static void screen_entropy_render(void);
static void screen_entropy_on_button(button_id_t btn);

static void screen_wipe_confirm_enter(void);
static void screen_wipe_confirm_render(void);
static void screen_wipe_confirm_on_button(button_id_t btn);

static void screen_lock_hold_enter(void);
static void screen_lock_hold_render(void);
static void screen_lock_hold_on_button(button_id_t btn);

static void screen_lock_confirm_render(void);
static void screen_lock_confirm_on_button(button_id_t btn);
static void screen_show_seed_confirm_render(void);
static void screen_show_seed_confirm_on_button(button_id_t btn);


static void screen_mnemonic_verify_enter(void);
static void screen_mnemonic_verify_render(void);
static void screen_mnemonic_verify_on_button(button_id_t btn);

static void screen_session_confirm_enter(void);
static void screen_session_confirm_render(void);
static void screen_session_confirm_on_button(button_id_t btn);

static void screen_passphrase_enter(void);
static void screen_passphrase_render(void);
static void screen_passphrase_on_button(button_id_t btn);

static void screen_temp_seed_enter(void);
static void screen_temp_seed_render(void);
static void screen_temp_seed_on_button(button_id_t btn);

static void screen_ble_name_enter(void);
static void screen_ble_name_render(void);
static void screen_ble_name_on_button(button_id_t btn);

static void screen_passphrase_confirm_enter(void);
static void screen_passphrase_confirm_render(void);
static void screen_passphrase_confirm_on_button(button_id_t btn);

/* Secret-clearing exit hooks (AUDIT S5); defined with the buffers they own. */
static void forget_mnemonic_unless_needed(screen_id_t next);
static void forget_mnemonic_entry(screen_id_t next);
static void forget_pin_entry(screen_id_t next);
static void forget_passphrase_entry(screen_id_t next);

static void screen_sd_seed_enter(void);
static void screen_sd_seed_exit(screen_id_t next);
static void screen_sd_seed_render(void);
static void screen_sd_seed_on_button(button_id_t btn);
static void screen_show_address_enter(void);
static void screen_show_address_exit(screen_id_t next);
static void screen_show_address_render(void);
static void screen_show_address_on_button(button_id_t btn);
static void screen_scan_enter(void);
static void screen_scan_render(void);
static void screen_scan_on_button(button_id_t btn);
static void screen_scan_exit(screen_id_t next);
static void screen_qr_out_render(void);
static void screen_qr_out_on_button(button_id_t btn);
static void screen_qr_out_enter(void);
static void screen_qr_out_exit(screen_id_t next);

/* Built-in screen definitions */
static const screen_t screen_boot = {
    .enter = screen_boot_enter,
    .render = screen_boot_render,
    .on_button = screen_boot_on_button,
    .exit = NULL
};

static const screen_t screen_pin_setup = {
    .enter = screen_pin_setup_enter,
    .render = screen_pin_setup_render,
    .on_button = screen_pin_setup_on_button,
    .exit = forget_pin_entry
};

static const screen_t screen_pin_unlock = {
    .enter = screen_pin_unlock_enter,
    .render = screen_pin_unlock_render,
    .on_button = screen_pin_unlock_on_button,
    .exit = forget_pin_entry
};

static void screen_pin_change_enter(void);
static void screen_pin_change_render(void);
static void screen_pin_change_on_button(button_id_t btn);
static void screen_pin_change_exit(screen_id_t next);

static const screen_t screen_pin_change = {
    .enter = screen_pin_change_enter,
    .render = screen_pin_change_render,
    .on_button = screen_pin_change_on_button,
    .exit = screen_pin_change_exit
};

static const screen_t screen_main_menu = {
    .enter = screen_main_menu_enter,
    .render = screen_main_menu_render,
    .on_button = screen_main_menu_on_button,
    .exit = NULL
};

static const screen_t screen_wallet_info = {
    .enter = screen_wallet_info_enter,
    .render = screen_wallet_info_render,
    .on_button = screen_wallet_info_on_button,
    .exit = NULL
};

static const screen_t screen_wallet_create = {
    .enter = screen_wallet_create_enter,
    .render = screen_wallet_create_render,
    .on_button = screen_wallet_create_on_button,
    .exit = forget_mnemonic_unless_needed
};

static const screen_t screen_wallet_select = {
    .enter = screen_wallet_select_enter,
    .render = screen_wallet_select_render,
    .on_button = screen_wallet_select_on_button,
    .exit = NULL
};

static const screen_t screen_mnemonic_display = {
    .enter = screen_mnemonic_display_enter,
    .render = screen_mnemonic_display_render,
    .on_button = screen_mnemonic_display_on_button,
    .exit = forget_mnemonic_unless_needed
};

static const screen_t screen_mnemonic_entry = {
    .enter = screen_mnemonic_entry_enter,
    .render = screen_mnemonic_entry_render,
    .on_button = screen_mnemonic_entry_on_button,
    .exit = forget_mnemonic_entry
};

static const screen_t screen_settings = {
    .enter = screen_settings_enter,
    .render = screen_settings_render,
    .on_button = screen_settings_on_button,
    .exit = NULL
};

static const screen_t screen_entropy = {
    .enter = screen_entropy_enter,
    .render = screen_entropy_render,
    .on_button = screen_entropy_on_button,
    .exit = NULL
};

static const screen_t screen_wipe_confirm = {
    .enter = screen_wipe_confirm_enter,
    .render = screen_wipe_confirm_render,
    .on_button = screen_wipe_confirm_on_button,
    .exit = NULL
};

static const screen_t screen_lock_hold = {
    .enter = screen_lock_hold_enter,
    .render = screen_lock_hold_render,
    .on_button = screen_lock_hold_on_button,
    .exit = NULL
};

static const screen_t screen_lock_confirm = {
    .enter = NULL,
    .render = screen_lock_confirm_render,
    .on_button = screen_lock_confirm_on_button,
    .exit = NULL
};

static const screen_t screen_show_seed_confirm = {
    .enter = NULL,
    .render = screen_show_seed_confirm_render,
    .on_button = screen_show_seed_confirm_on_button,
    .exit = NULL
};

static const screen_t screen_mnemonic_verify = {
    .enter = screen_mnemonic_verify_enter,
    .render = screen_mnemonic_verify_render,
    .on_button = screen_mnemonic_verify_on_button,
    .exit = forget_mnemonic_unless_needed
};

static const screen_t screen_session_confirm = {
    .enter = screen_session_confirm_enter,
    .render = screen_session_confirm_render,
    .on_button = screen_session_confirm_on_button,
    .exit = NULL
};

static const screen_t screen_passphrase = {
    .enter = screen_passphrase_enter,
    .render = screen_passphrase_render,
    .on_button = screen_passphrase_on_button,
    .exit = forget_passphrase_entry
};

static const screen_t screen_temp_seed = {
    .enter = screen_temp_seed_enter,
    .render = screen_temp_seed_render,
    .on_button = screen_temp_seed_on_button,
    .exit = NULL
};

static const screen_t screen_ble_name = {
    .enter = screen_ble_name_enter,
    .render = screen_ble_name_render,
    .on_button = screen_ble_name_on_button,
    .exit = NULL
};

static const screen_t screen_passphrase_confirm = {
    .enter = screen_passphrase_confirm_enter,
    .render = screen_passphrase_confirm_render,
    .on_button = screen_passphrase_confirm_on_button,
    .exit = NULL
};

static const screen_t screen_sd_seed = {
    .enter = screen_sd_seed_enter,
    .render = screen_sd_seed_render,
    .on_button = screen_sd_seed_on_button,
    .exit = screen_sd_seed_exit
};

static const screen_t screen_show_address = {
    .enter = screen_show_address_enter,
    .render = screen_show_address_render,
    .on_button = screen_show_address_on_button,
    .exit = screen_show_address_exit
};

static const screen_t screen_scan = {
    .enter = screen_scan_enter,
    .render = screen_scan_render,
    .on_button = screen_scan_on_button,
    .exit = screen_scan_exit
};

static const screen_t screen_qr_out = {
    .enter = screen_qr_out_enter,
    .render = screen_qr_out_render,
    .on_button = screen_qr_out_on_button,
    .exit = screen_qr_out_exit
};

/* ============================================================================
 * PIN Entry State
 * ============================================================================ */

#define PIN_DISPLAY_LEN PIN_MAX_LENGTH

/* The selector cycles 0-9 plus a submit option, so ACCEPT means "append the
 * highlighted digit" and submitting is a separate, deliberate act. Without it
 * the fourth digit doubled as submit and no PIN longer than the minimum could
 * ever be entered. */
#define PIN_OPTION_SUBMIT 10
#define PIN_OPTION_COUNT  11

static char pin_entry[PIN_MAX_LENGTH + 1] = {0};
static int pin_cursor = 0;
static bool pin_confirm_mode = false;
static char pin_first_entry[PIN_MAX_LENGTH + 1] = {0};
static int current_digit = 0;

/* Submit is only offered once the PIN is long enough to be valid. */
static bool pin_can_submit(void)
{
    return pin_cursor >= PIN_MIN_LENGTH;
}

/* PICKER_FORWARD: in every "prev <cur> next" picker - PIN digits, seed and
 * passphrase letters, the BLE name, the dice face - DOWN moves forward (to
 * the right-hand neighbour, the digit one higher) and UP moves back. DOWN is
 * what a clockwise click of the knob sends, and clockwise should increase,
 * as it is already "next" in every menu and review. Physical buttons follow
 * the same rule, so DN now raises a digit. To restore the old sense, swap
 * the signs at each call site marked PICKER_FORWARD. */
static void pin_option_scroll(int dir)
{
    int n = pin_can_submit() ? PIN_OPTION_COUNT : 10;
    if (current_digit >= n) {
        current_digit = 0;
    }
    current_digit = ((current_digit + dir) % n + n) % n;
}

static void pin_option_text(int idx, char *out, size_t max)
{
    if (idx == PIN_OPTION_SUBMIT) {
        snprintf(out, max, "OK");
    } else {
        snprintf(out, max, "%d", idx);
    }
}

/* Render the selector with its neighbours, e.g. "8 <9> OK".
 *
 * Showing what comes next is what makes OK findable. With only the current
 * option on screen, OK sits one step past 9 with nothing hinting it exists, and
 * a user who never scrolls past 9 has no way to submit at all. */
static void pin_option_label(char *out, size_t max)
{
    int n = pin_can_submit() ? PIN_OPTION_COUNT : 10;
    if (current_digit >= n) {
        current_digit = 0;
    }

    char prev[4], cur[4], next[4];
    pin_option_text(((current_digit - 1) % n + n) % n, prev, sizeof(prev));
    pin_option_text(current_digit, cur, sizeof(cur));
    pin_option_text((current_digit + 1) % n, next, sizeof(next));

    snprintf(out, max, "%s <%s> %s", prev, cur, next);
}

static void pin_entry_reset(void)
{
    memzero(pin_entry, sizeof(pin_entry));
    pin_cursor = 0;
    current_digit = 0;
}

static void pin_entry_add_digit(int digit)
{
    if (pin_cursor < PIN_MAX_LENGTH) {
        pin_entry[pin_cursor] = '0' + digit;
        pin_cursor++;
        pin_entry[pin_cursor] = '\0';
    }
}

static void pin_entry_backspace(void)
{
    if (pin_cursor > 0) {
        pin_cursor--;
        pin_entry[pin_cursor] = '\0';
    }
}

/* ============================================================================
 * Menu State
 * ============================================================================ */

/* The menu is built per render rather than fixed.
 *
 * One seed is the recommended configuration (docs/VAULT.md), so "Select Wallet"
 * is noise until a second one exists - and a menu entry that does nothing is
 * worse than absent on a four-button device where every scroll costs a press. */
typedef enum {
    MENU_VIEW_ADDRESS,
    MENU_SELECT_WALLET,
    MENU_NEW_WALLET,
    MENU_IMPORT_WALLET,
    /* Temporary seed (T69). A menu action rather than a settings toggle, and
     * that is a security decision, not a layout one: a toggle leaves the device
     * in a state where stored wallets and a typed seed both exist and nothing
     * on screen settles which one is being signed with. Entering the mode is an
     * act with a beginning and an end, so it is a verb in the menu. */
    MENU_TEMP_SEED,
    /* And the way out, offered only while the mode is on. It locks - which is
     * the only way out there is, because ending temporary mode means destroying
     * the seed, and destroying the seed is what locking does. */
    MENU_END_TEMP,
    /* The air gap. Show address is on every board: it needs nothing but the
     * panel. Scan needs a camera and is simply absent without one, rather
     * than present and apologising; it takes ur:leek-pczt (airgap.h). */
    MENU_SHOW_ADDRESS,
    MENU_SCAN,
    /* SD storage mode (roadmap 1.4): load a .zkv seed for the session, or
     * save the open one. On every board with a card slot, seed or no seed:
     * loading one is how a device with nothing stored gets a wallet. */
    MENU_SD_SEED,
    MENU_SETTINGS,
    MENU_ACTION_COUNT
} MenuAction;

#define MENU_MAX_ITEMS MENU_ACTION_COUNT

static MenuAction menu_actions[MENU_MAX_ITEMS];
static int menu_item_count = 0;
static int menu_selection = 0;

static const char *menu_action_label(MenuAction a)
{
    switch (a) {
        case MENU_VIEW_ADDRESS:  return "Wallet Info";
        case MENU_SELECT_WALLET: return "Select Wallet";
        case MENU_NEW_WALLET:    return "New Wallet";
        case MENU_IMPORT_WALLET: return "Import Wallet";
        case MENU_TEMP_SEED:     return "Temp Seed";
        case MENU_END_TEMP:      return "End Temp Seed";
        case MENU_SHOW_ADDRESS:  return "Receive";
        case MENU_SCAN:          return "Scan";
        case MENU_SD_SEED:       return "SD Card";
        case MENU_SETTINGS:      return "Settings";
        default:                 return "?";
    }
}

static void menu_rebuild(void)
{
    WalletStatus status = wallet_get_status();
    menu_item_count = 0;

    const bool temp = wallet_has_temporary_mnemonic();

    if (status.wallet_count > 0 || temp) {
        menu_actions[menu_item_count++] = MENU_VIEW_ADDRESS;
    }
    if (status.wallet_count > 1 || (temp && status.wallet_count > 0)) {
        menu_actions[menu_item_count++] = MENU_SELECT_WALLET;
    }

    /* Both need a seed to speak for; with none there is nothing to share and
     * nothing that could sign. */
    if (status.wallet_count > 0 || temp) {
        menu_actions[menu_item_count++] = MENU_SHOW_ADDRESS;
#if LEEK_HAS_CAMERA
        menu_actions[menu_item_count++] = MENU_SCAN;
#endif
    }

    /* Unlike New/Import below, this stays on the home screen whatever is
     * stored. It is the whole feature: a user who wants to sign with a seed
     * this device must never keep should not have to go looking for the option
     * under Settings, and a careless press costs them a warning screen and a
     * seed phrase they would have to type in full. */
    if (temp) {
        menu_actions[menu_item_count++] = MENU_END_TEMP;
    } else {
        menu_actions[menu_item_count++] = MENU_TEMP_SEED;
    }
#if UI_HAS_SD_SEED
    menu_actions[menu_item_count++] = MENU_SD_SEED;
#endif

    /* Creating or importing a seed is only a top-level action on a device that
     * has none. Once one exists, those entries sit one careless press from
     * generating a wallet the user then mistakes for theirs, and no established
     * hardware wallet offers them from the home screen either - you reset the
     * device instead. They remain available under Settings. */
    if (status.wallet_count == 0) {
        menu_actions[menu_item_count++] = MENU_NEW_WALLET;
        menu_actions[menu_item_count++] = MENU_IMPORT_WALLET;
    }

    menu_actions[menu_item_count++] = MENU_SETTINGS;

    if (menu_selection >= menu_item_count) {
        menu_selection = menu_item_count - 1;
    }
    if (menu_selection < 0) {
        menu_selection = 0;
    }
}

/* ============================================================================
 * Wallet State
 * ============================================================================ */

/* Which BIP44 account of the active seed the device's own screens are on
 * (T45): m/44'/<coin>'/<hd_account>'.
 *
 * Persisted, because the account is *which identity is in use* — the same
 * role active_idx plays for seeds — and a device that silently reverts to
 * account 0 after a reboot shows a different wallet for the same seed, which
 * reads as funds having vanished. Nothing derives at it yet: the old
 * address browser that did is gone, and the Zcash one is a later phase.
 *
 * Bounded by HD_ACCOUNT_COUNT on this device's own screens; see ui.h for why
 * the bound is a UI bound and not a derivation one. */
static uint32_t hd_account = 0;

/* The board's network: what the receive screens show addresses for, and the
 * only one a companion's requests are answered for (see UI_KEY_NETWORK).
 * Written by the UI task only; read by the protocol task through
 * ui_zcash_network(). One byte, so the read never tears; volatile, so the
 * protocol task never acts on a stale copy after the user switched it. */
static volatile uint8_t zcash_network = 0;

#if LEEK_HAS_ZCASH
/* The transaction review (SCREEN_ZSIGN_REVIEW). Up here because two flows
 * open it: a host's zSignFinish, and a PCZT read from the SD card. */
static const zs_summary *zsign_summary = NULL;
static uint8_t zsign_network = ZS_NET_TEST;
static int zsign_page = 0;
/* True when the review on screen came from the SD card: its answer goes to
 * sd_sign_review_done() rather than to a waiting host. */
static bool zsign_from_sd = false;
/* Likewise for a PCZT scanned off the companion's screen (ur:leek-pczt):
 * the answer goes to qr_sign_review_done(), which shows ur:leek-zsig. */
static bool zsign_from_qr = false;
static void qr_sign_review_done(bool approved);
#endif

static char mnemonic_buffer[256];
static int mnemonic_word_count = 0;

/* Set when the seed display is gated behind a fresh PIN entry. Declared here
 * because auto-lock clears it, and auto-lock is defined above its old home. */
static bool pending_mnemonic_display = false;
static int mnemonic_page = 0;  /* Current page (3 words per page) */
static int wallet_list_selection = 0;

/* Create wallet state */
static int create_word_count = 12;  /* 12 or 24 */
/* Set when the entropy screen has met the gate for the chosen length, so the
 * create screen knows it is being re-entered to finish rather than to ask
 * again. Cleared by the menu on the way in, which is the only path that starts
 * a new wallet. */
static bool create_entropy_ready = false;
static bool create_show_mnemonic = false;
static char create_error[32] = {0};

/* Set by the button handler, consumed by ui_poll_deferred() (AUDIT S8f).
 *
 * Generating a seed unlocks the vault and runs BIP39 — a second or more on
 * this silicon, with the UI task blocked throughout — so the user has to be
 * told it started. The old code told them by calling ui_render() from inside
 * the button handler: a second render path into a screen that was already
 * mid-transition, so a repaint arriving from anywhere else could redraw a
 * screen whose state had half-changed, and the flush ordering depended on
 * which of the two paths ran last. Setting a flag instead keeps exactly one
 * render path — the loop's — and the work happens after the frame is on the
 * panel, which is what the ui_render() call was reaching for. */
static bool create_generate_pending = false;

/* Mnemonic entry state - logic lives in mnemonic-entry.c so it can be tested
 * on the host without an ESP32 attached (see sim/). */
static MnemonicEntry entry;
static char entry_error[20] = {0};

/* Import starts by asking how long the phrase is (T3, AUDIT S8d).
 *
 * The entry logic has always handled 24 words; there was simply no way to say
 * so, which made every 24-word backup unimportable. Asking first rather than
 * inferring: a 24-word phrase typed into a 12-word target silently imports the
 * wrong wallet at word 12, and the checksum makes that look like a typing
 * error rather than the wrong question. */
static bool entry_choosing_length = true;
static int  entry_length_choice = 12;

/* Whether the phrase being typed is destined for storage or for this session
 * only (T69). The two flows are the same keystrokes over the same buffers and
 * differ in exactly one place - what mnemonic_entry_finish() hands the phrase
 * to - so they share a screen rather than being copied. The screen still says
 * which one it is on every frame, because the difference between them is
 * whether the user's seed survives the next reboot. */
static bool entry_is_temporary = false;

/* ============================================================================
 * Leaving a screen that held a secret (AUDIT S5, T6)
 *
 * `wallet_lock()` is careful with the seed; the UI layer above it was not. The
 * plaintext mnemonic sat in `mnemonic_buffer` from the moment it was displayed
 * until some later screen happened to overwrite it - across lock, across a
 * wipe, and into any crash dump or JTAG pause taken in between. "Wipe Device"
 * left the seed in RAM until the next reboot.
 *
 * These hooks close that window at the only moment that is both well-defined
 * and cheap: the screen transition.
 * ============================================================================ */

/* The seed-creation flow hands the buffer back and forth between showing the
 * words and checking the user wrote them down. Within that group the mnemonic
 * is still live; leaving it, it is not. */
static bool mnemonic_still_needed_by(screen_id_t next)
{
    return next == SCREEN_MNEMONIC_DISPLAY || next == SCREEN_MNEMONIC_VERIFY;
}

static void forget_mnemonic_unless_needed(screen_id_t next)
{
    if (mnemonic_still_needed_by(next)) {
        return;
    }
    memzero(mnemonic_buffer, sizeof(mnemonic_buffer));
    mnemonic_word_count = 0;
    mnemonic_page = 0;

    /* And the copy nobody in this file owns. mnemonic_from_data() builds the
     * phrase in a file-scope `static CONFIDENTIAL char mnemo[240]` inside
     * bip39.c and returns a pointer into it; wallet_create_mnemonic() copies
     * out of that pointer, so every generated seed leaves a second plaintext
     * copy behind. mnemonic_clear() exists to wipe it and was called from
     * nowhere in this firmware, which left a freshly generated phrase sitting
     * in RAM until reboot - through lock, through wipe, into any crash dump or
     * JTAG halt taken in between.
     *
     * Here is the right place rather than immediately after generation, for the
     * same reason mnemonic_buffer is cleared here: this hook is the one moment
     * that is both well-defined and known to be past the last reader. The
     * import path reaches it too, which is free - clearing a buffer that
     * already holds zeros costs nothing and means no future caller of
     * mnemonic_from_data() can reintroduce the leak by taking a different route
     * out of these screens. */
    mnemonic_clear();
}

/* The import screen's word buffer. Nothing downstream reads it - the built
 * mnemonic goes straight into the wallet and is zeroed there - so this can go
 * unconditionally. */
static void forget_mnemonic_entry(screen_id_t next)
{
    (void)next;
    mnemonic_entry_clear(&entry);
    memzero(entry_error, sizeof(entry_error));
    entry_choosing_length = true;
    entry_is_temporary = false;
}

/* The digits as typed. `pin.c` keeps its own verified copy for wallet
 * encryption and clears that on lock; this is the UI's transcript of the
 * keypresses and no one needs it after the screen is gone. */
static void forget_pin_entry(screen_id_t next)
{
    (void)next;
    memzero(pin_entry, sizeof(pin_entry));
    memzero(pin_first_entry, sizeof(pin_first_entry));
    pin_cursor = 0;
    current_digit = 0;
    pin_confirm_mode = false;
}

/* ============================================================================
 * Auto-lock
 *
 * Locking clears secrets, not preferences. Two kinds of state exist after an
 * unlock and they must be treated differently:
 *
 *   Secret   - passphrase, decrypted mnemonic, cached seed. Cleared on lock,
 *              because that is what locking is for. Keeping the passphrase
 *              across a lock would mean the PIN alone reopens a hidden wallet,
 *              which removes the second factor entirely.
 *
 *   Selection - which wallet, which address index. Not secret, already stored
 *              in NVS, and losing it on every lock is pure annoyance. Kept.
 * ============================================================================ */

/* 1, 5, 10 or 30 minutes. Trezor and Ledger both default to 10; 5 is chosen
 * here because a device meant for cold storage is idle far more often than it
 * is used, and the cost of being wrong in that direction is one PIN entry.
 *
 * There is deliberately no "never". A wallet that stays unlocked indefinitely
 * is a footgun, and 30 minutes is long enough for bench work. */
static const uint32_t LOCK_TIMEOUT_CHOICES[] = { 60, 300, 600, 1800 };
#define LOCK_TIMEOUT_COUNT (sizeof(LOCK_TIMEOUT_CHOICES) / sizeof(LOCK_TIMEOUT_CHOICES[0]))

static int      lock_timeout_choice = 1;   /* default 5 minutes */

/* What NVS holds, which is not always what is in force (T69).
 *
 * A temporary seed changes what a lock costs. With a stored wallet, locking
 * costs one PIN entry; with a seed that exists only in RAM, it costs the seed
 * and up to 24 words to type it again. So temporary mode runs on a longer
 * default - and only temporary mode does, which is why the override lives in
 * a second variable instead of being written to flash. Nothing about a stored
 * wallet's timeout changes, and the user's own choice comes back untouched the
 * moment the temporary seed is gone. */
static int      lock_timeout_stored_choice = 1;

/* 30 minutes, the longest offer, as this mode's default. Chosen by the owner:
 * re-entering a 24-word phrase every five idle minutes is the kind of friction
 * that ends with the seed being stored after all. Every other choice stays
 * reachable in Settings while the mode is on. */
#define LOCK_TIMEOUT_TEMP_SEED 3

static int64_t  last_activity_us = 0;

/* Settings are persisted separately from the vault: they are not secret, and
 * they must survive a wipe of neither more nor less than the wallet does. */
#define UI_NVS_NAMESPACE "leek_ui"
#define UI_KEY_LOCK_TIMEOUT "lock_to"
#define UI_KEY_BRIGHTNESS   "bright"
#define UI_KEY_ENTRY_BLOCKS "wblocks"
/* The selected BIP44 account (T45). Not secret — it is in every watch-only
 * descriptor — and stored beside the other selections rather than in the
 * vault, so it survives a lock exactly as the active wallet index does. */
#define UI_KEY_ACCOUNT      "acct"
/* Which Zcash network the receive screens derive for (ZS_NET_*: 0 main,
 * 1 test). Not secret, and only a view: the signer takes the network a PCZT
 * declares, never this. */
#define UI_KEY_NETWORK      "zc_net"
/* Settings > Knob: 1 when encoder turns are reversed (UP and DOWN swapped for
 * the knob only). XORed with the board's BOARD_ENC_REVERSE in button.c, so a
 * board wired the other way round is fixed at compile time and this stays a
 * matter of taste. Absent or any other value is Normal. */
#define UI_KEY_KNOB_REVERSE "knob_rev"

/* Only a board with an encoder offers the setting; on the others there is
 * nothing for it to turn. The host tests ask for it explicitly so the menu
 * item can be exercised without building for that board. */
#ifndef UI_HAS_KNOB_SETTING
#define UI_HAS_KNOB_SETTING LEEK_HAS_ENCODER
#endif

/* Four steps rather than a slider: the OLED is legible across the whole range,
 * so fine control buys nothing and costs presses. Low is genuinely useful -
 * a dim screen is harder to read over someone's shoulder. */
/*
 * Indices are what NVS stores, so they only ever grow at the end: Min and Dim
 * were added after the first four had shipped, and putting them in front would
 * have turned every saved "Low" into "Min". The order a user steps through is
 * BRIGHTNESS_CYCLE, which is dimmest to brightest regardless of storage order.
 *
 * Min and Dim exist for two reasons. A dark room, where Low is still a lamp.
 * And a camera reading a QR off this panel: at full brightness an OLED blooms
 * on a webcam's sensor, the lit pixels bleed into the dark modules, and a
 * dimmer panel can be the one that decodes.
 */
static const uint8_t BRIGHTNESS_LEVELS[] = { 0x10, 0x50, 0xA0, 0xFF, 0x00, 0x04 };
#define BRIGHTNESS_COUNT (sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]))
static const uint8_t BRIGHTNESS_CYCLE[] = { 4, 5, 0, 1, 2, 3 };
/* Dim by default: an OLED is bright in a dark room, and it is the level a
   phone camera reads this panel's QR codes best at. */
static int brightness_choice = 5;
/* The QR screen's own level, stepped with OK; see screen_qr_out_enter. */
static int qr_brightness = 5;

static const char *brightness_label(int choice)
{
    switch (choice) {
        case 0:  return "Low";
        case 1:  return "Mid";
        case 2:  return "High";
        case 3:  return "Max";
        case 4:  return "Min";
        case 5:  return "Dim";
        default: return "?";
    }
}

/* The level after `choice` in dimmest-to-brightest order, wrapping. */
static int brightness_next(int choice)
{
    for (size_t i = 0; i < BRIGHTNESS_COUNT; i++) {
        if (BRIGHTNESS_CYCLE[i] == choice) {
            return BRIGHTNESS_CYCLE[(i + 1) % BRIGHTNESS_COUNT];
        }
    }
    return 2;
}

/* One setting, both selectors (T60).
 *
 * The seed-word selector and the passphrase selector are the same two-level
 * control over different alphabets, and a mode on four unlabelled buttons is a
 * real cost. A user who has decided how they want to be asked for letters has
 * decided it for both screens, so "Entry: Simple/Blocks" drives both and there
 * is deliberately no second preference to get out of step with this one. */
/* The setting drives the seed selector only.
 *
 * Blocks are measurably faster on both screens - 9.1 presses per character
 * against 14.4 on the passphrase ring - and were reported worse to use there
 * anyway. The reason they differ is structural rather than a matter of taste.
 * Seed entry offers one dimension: 26 letters, narrowing as you type. The
 * passphrase ring already has a mode dimension on top of its ~95 characters,
 * so adding blocks makes it three levels of navigation on four unlabelled
 * buttons - open a block, pick a character, and separately remember which
 * character set you are in.
 *
 * So the passphrase keeps the flat ring, which still improved: it now offers
 * the two sets you are NOT in, which is where most of its gain came from.
 * Fewer presses is not the only axis, and a passphrase typed wrong is worse
 * than a passphrase typed slowly. */
static void entry_blocks_apply(bool enabled)
{
    mnemonic_entry_set_blocks(enabled);
    text_entry_set_blocks(false);
}

static void settings_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(UI_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;   /* never saved; keep the defaults */
    }

    uint8_t stored = 0;
    if (nvs_get_u8(nvs, UI_KEY_LOCK_TIMEOUT, &stored) == ESP_OK &&
        stored < LOCK_TIMEOUT_COUNT) {
        lock_timeout_choice = (int)stored;
        lock_timeout_stored_choice = (int)stored;
    }
    if (nvs_get_u8(nvs, UI_KEY_BRIGHTNESS, &stored) == ESP_OK &&
        stored < BRIGHTNESS_COUNT) {
        brightness_choice = (int)stored;
    }
    if (nvs_get_u8(nvs, UI_KEY_ENTRY_BLOCKS, &stored) == ESP_OK) {
        entry_blocks_apply(stored != 0);
    }
    /* Range-checked on the way in, not just on the way out. A byte outside the
     * menu's range would leave the device deriving from an account no screen
     * can reach or re-select, which is precisely the "shows a different wallet
     * than you think" failure the path row exists to prevent. */
    if (nvs_get_u8(nvs, UI_KEY_ACCOUNT, &stored) == ESP_OK &&
        stored < HD_ACCOUNT_COUNT) {
        hd_account = stored;
    }
    if (nvs_get_u8(nvs, UI_KEY_NETWORK, &stored) == ESP_OK && stored <= 1) {
        zcash_network = stored;
    }
    if (nvs_get_u8(nvs, UI_KEY_KNOB_REVERSE, &stored) == ESP_OK) {
        button_set_knob_reversed(stored == 1);
    }
    nvs_close(nvs);
}

/* Flipped and saved in one step, and effective on the next detent: button.c
 * reads the flag on every turn, so there is nothing to restart. */
static void knob_reverse_toggle(void)
{
    const bool reversed = !button_knob_reversed();
    button_set_knob_reversed(reversed);

    nvs_handle_t nvs;
    if (nvs_open(UI_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, UI_KEY_KNOB_REVERSE, reversed ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static void brightness_apply_and_save(void)
{
    oled_set_contrast(BRIGHTNESS_LEVELS[brightness_choice]);

    nvs_handle_t nvs;
    if (nvs_open(UI_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, UI_KEY_BRIGHTNESS, (uint8_t)brightness_choice);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/* Which selector the seed-entry screen uses. Persisted because the answer is a
 * preference about how the buttons behave, and re-choosing it every time would
 * be its own small tax. */
static void entry_blocks_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open(UI_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, UI_KEY_ENTRY_BLOCKS,
                   mnemonic_entry_blocks_enabled() ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/* Persisting the account, and resetting the address index with it.
 *
 * The reset is not politeness: index 3 of account 0 and index 3 of account 1
 * are unrelated addresses, so carrying the index across a change would leave
 * the screen at an address the user never chose in an account they just
 * entered. Callers change the account through here for that reason. */
uint32_t ui_hd_account(void)
{
    return hd_account;
}

uint8_t ui_zcash_network(void)
{
    return zcash_network;
}

/* A request for the other network was refused (ui_notify_wrong_network).
 * Pending: the network the companion asked for, plus one; 0 for none. Set
 * by the protocol task, taken by the UI task, which then shows the notice
 * until wrong_network_until_us. */
#define WRONG_NETWORK_NOTICE_US (4 * 1000 * 1000)
static volatile uint8_t wrong_network_pending = 0;
static uint8_t wrong_network_asked;
static int64_t wrong_network_until_us = 0;

void ui_notify_wrong_network(uint8_t asked)
{
    wrong_network_pending = (uint8_t)(asked + 1);
    ui_invalidate();
}

static void service_wrong_network(void)
{
    const uint8_t pending = wrong_network_pending;
    if (pending) {
        wrong_network_pending = 0;
        wrong_network_asked = (uint8_t)(pending - 1);
        wrong_network_until_us = esp_timer_get_time() + WRONG_NETWORK_NOTICE_US;
        ui_invalidate();
    } else if (wrong_network_until_us && esp_timer_get_time() >= wrong_network_until_us) {
        wrong_network_until_us = 0;
        ui_invalidate();
    }
}

/* Drawn instead of the screen while it lasts; the screen comes back as it
 * was. Short on purpose: the companion shows the whole sentence. */
static void render_wrong_network(void)
{
    oled_clear();
    oled_draw_string_centered(0, "Wrong network");
    oled_draw_string_centered(2, wrong_network_asked ? "App: Testnet" : "App: Mainnet");
    oled_draw_string_centered(3, zcash_network ? "Board: Testnet" : "Board: Mainnet");
    oled_draw_string_centered(5, "Refused. Change it");
    oled_draw_string_centered(6, "Settings > Network");
    oled_draw_string(7, 0, HINT_OK);
}

static void zcash_network_toggle(void)
{
    zcash_network = zcash_network ? 0 : 1;

    nvs_handle_t nvs;
    if (nvs_open(UI_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "Could not persist the network selection");
        return;
    }
    nvs_set_u8(nvs, UI_KEY_NETWORK, zcash_network);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void hd_account_set(uint32_t account)
{
    hd_account = account % HD_ACCOUNT_COUNT;

    nvs_handle_t nvs;
    if (nvs_open(UI_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "Could not persist the account selection");
        return;
    }
    nvs_set_u8(nvs, UI_KEY_ACCOUNT, (uint8_t)hd_account);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* The cached BIP32 master fingerprint, as eight hex characters, or "" when
 * there is none to show. Lives up here because locking has to be able to drop
 * it; the reasoning about what it is for is with refresh_master_xfp(). */
static char master_xfp[9] = {0};

/* The short ZIP 32 seed fingerprint (zcash-receive.h), or "". Lives and dies
 * with master_xfp: both name the seed, so they are read and cleared together.
 * It is the one a Zcash companion can show too (zGetAccount's
 * seedFingerprint), which makes it the one worth comparing. */
static char master_zfp[9] = {0};

/* Whether a passphrase was applied when the fingerprint currently on the
 * wallet screen was derived (T42).
 *
 * The screen derives on entry and then sits there. A host can set or clear a
 * passphrase over the wire, and a lock can drop one, while it sits — leaving
 * an XFP that belongs to a wallet that is no longer selected. That is the
 * stale-passphrase failure in its most direct form: the user reads an
 * identifier off a screen the device would no longer produce. So the
 * derived-at state is recorded here and checked by ui_poll_deferred(). */
static bool wallet_info_passphrase_shown = false;

/* Lock the device. The ONLY way this file locks anything.
 *
 * Reported from hardware: the user pressed "Lock device" on the main menu,
 * entered their PIN again, and was still in their passphrase wallet. The menu
 * item called pin_lock() and nothing else, so the PIN gate closed over a vault
 * that stayed open - passphrase, decrypted mnemonic, cached seed and derived
 * node all still in RAM and still applied. Auto-lock and the host lock did
 * call wallet_lock(), which is why it survived: the lock a user reaches for
 * deliberately was the one that did not lock.
 *
 * Each of pin_lock() and wallet_lock() was individually correct. What was
 * wrong was that four call sites each had to remember both, and one did not.
 * So there is now one function, it is what every path calls, and "locked"
 * means the same thing everywhere:
 *
 *   - the PIN gate is closed (pin_lock)
 *   - the vault is shut and the passphrase, mnemonic and seed cache are zeroed
 *     (wallet_lock) - this is the T42 rule, because keeping a passphrase
 *     across a lock means the PIN alone reopens a hidden wallet
 *   - the UI's own copies of the seed go with them
 *   - the cached fingerprint goes too. It names the seed the passphrase
 *     produced, and a fingerprint outliving its passphrase is the same bug in
 *     a quieter form: the next screen to draw it would vouch for a wallet the
 *     device can no longer derive.
 *
 * What survives is what a lock is not for: which wallet and which account.
 * Both are selections, not secrets, and both are already in NVS. */
static void lock_device(void)
{
    pin_lock();
    wallet_lock();

    pending_mnemonic_display = false;
    memzero(mnemonic_buffer, sizeof(mnemonic_buffer));
    mnemonic_word_count = 0;
    mnemonic_page = 0;

    memzero(master_xfp, sizeof(master_xfp));
    memzero(master_zfp, sizeof(master_zfp));
    wallet_info_passphrase_shown = false;
#if LEEK_HAS_ZCASH
    /* Session-only metadata belongs to seeds the lock just destroyed. */
    zmeta_forget_session();
#endif
#if UI_HAS_SD_SEED
    /* A card transaction under review is not signed by whoever unlocks next. */
    sdsign_cancel();
    zsign_from_sd = false;
    airgap_cancel();
    zsign_from_qr = false;
#endif

    /* wallet_lock() has just destroyed any temporary seed, so the longer
     * timeout it justified goes with it. The user's own preference is what is
     * left, and it was never overwritten. */
    lock_timeout_choice = lock_timeout_stored_choice;

    /* And the channel goes too.
     *
     * Locking used to leave the encrypted session up: the wallet closed, the
     * host kept talking, and a user who came back and entered their PIN
     * resumed on a session they had authorised before the lock -- possibly
     * hours before, with the device unattended in between. Seen on hardware,
     * where the companion carried on polling for two minutes after an
     * auto-lock.
     *
     * A lock is the deliberate hammer, so it should mean what a user assumes:
     * wallet away, channel closed, and a returning host compares six fresh
     * digits. That costs a handshake and never a retyped seed phrase -- which
     * is precisely why this belongs here and not in the idle timeout, whose
     * whole point is to be the weaker of the two.
     *
     * Here rather than at the five call sites because they have disagreed
     * before: this one shipped calling pin_lock() alone and left the vault
     * open underneath the PIN gate. */
    session_reset();
}

static void lock_timeout_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open(UI_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "Could not persist the auto-lock setting");
        return;
    }
    nvs_set_u8(nvs, UI_KEY_LOCK_TIMEOUT, (uint8_t)lock_timeout_choice);
    nvs_commit(nvs);
    nvs_close(nvs);

    /* Whatever the user picks becomes their standing preference, including one
     * picked while a temporary seed is in use. Otherwise the restore on lock
     * would quietly undo a setting they had just chosen. */
    lock_timeout_stored_choice = lock_timeout_choice;
}

static const char *lock_timeout_label(int choice)
{
    switch (choice) {
        case 0:  return "1 min";
        case 1:  return "5 min";
        case 2:  return "10 min";
        case 3:  return "30 min";
        default: return "?";
    }
}

static void lock_note_activity(void)
{
    last_activity_us = esp_timer_get_time();
}

/* Called from the UI task. Returns true if the device just auto-locked. */
static bool lock_check_timeout(void)
{
    uint32_t seconds = LOCK_TIMEOUT_CHOICES[lock_timeout_choice];
    if (!pin_is_unlocked()) {
        return false;
    }

    int64_t idle_us = esp_timer_get_time() - last_activity_us;
    if (idle_us < (int64_t)seconds * 1000000) {
        return false;
    }

    ESP_LOGI(TAG, "Auto-lock after %u s idle", (unsigned)seconds);
    /* The account and the active wallet survive deliberately; everything
     * else lock_device() drops. */
    lock_device();
    ui_set_screen(SCREEN_PIN_UNLOCK);
    return true;
}

/* Settings, ordered by how often they are wanted and how much they cost when
 * chosen by accident: wallet actions first, diagnostics next, wipe last. */
typedef enum {
    SET_SHOW_SEED,
    SET_NEW_WALLET,
    SET_IMPORT_WALLET,
    SET_PASSPHRASE,
    /* Next to the passphrase deliberately (T45). Both answer the same question
     * — "which wallet am I in?" — and the wallet screen shows the result. */
    SET_ACCOUNT,
    SET_NETWORK,
    SET_BRIGHTNESS,
    SET_AUTOLOCK,
    SET_ENTRY_STYLE,
    /* Beside Word entry: both are about how the controls behave. */
#if UI_HAS_KNOB_SETTING
    SET_KNOB,
#endif
    SET_CHANGE_PIN,
/* The Wi-Fi AP is a test fixture, not a feature (AUDIT S8g). It brings up an
 * access point with a hardcoded password on a device holding seeds, so it is
 * compiled out unless a build asks for it. The enum and the label array below
 * are positional and guarded together, which is what keeps them in step. */
#ifdef CONFIG_ESP_WIFI_ENABLED
    SET_WIFI,
#endif
    SET_TRANSPORT,
    SET_BLE_NAME,
    SET_USB,
    /* No "Blind sign" entry: it governed Ethereum calls the device could not
     * decode, and the Zcash firmware signs no Ethereum at all (ROADMAP 1.1).
     * The setting and its warning screen stay in the code, unreachable. */
    SET_WIPE,
    SET_BACK,
    SETTINGS_ITEMS
} SettingsAction;

static const char *settings_items[SETTINGS_ITEMS] = {
    "Show Seed",
    "New Wallet",
    "Import Wallet",
    "Passphrase",
    "Account",
    "Network",
    "Brightness",
    "Auto-lock",
    "Word entry",
#if UI_HAS_KNOB_SETTING
    "Knob direction",
#endif
    "Change PIN",
#ifdef CONFIG_ESP_WIFI_ENABLED
    "WiFi Test",
#endif
    "Link",
    "BLE Name",
    "USB HID Test",
    "Wipe Device",
    "Back"
};
static int settings_selection = 0;
static bool wifi_enabled = false;

/* ============================================================================
 * Helper: Unlock wallet with PIN
 * ============================================================================ */

static bool ensure_wallet_unlocked(void)
{
    WalletStatus status = wallet_get_status();

    /* Reinitialize if not initialized (e.g., after wipe) */
    if (!status.initialized) {
        ESP_LOGI(TAG, "Wallet not initialized, initializing...");
        wallet_init();
        status = wallet_get_status();
    }

    if (!status.unlocked) {
        char pin[PIN_MAX_LENGTH + 1];
        if (!pin_get_current(pin, sizeof(pin))) {
            ESP_LOGE(TAG, "No PIN available");
            return false;
        }

        /* First time: set password, subsequent: unlock. The stack copy is
         * zeroed on every path out: pin.c keeps the one authoritative copy and
         * clears it on lock, and a second one left in this frame would outlive
         * that lock in whatever called us next. */
        if (!status.password_set) {
            WalletError err = wallet_set_password(pin, strlen(pin));
            if (err != WALLET_OK) {
                ESP_LOGE(TAG, "Failed to set wallet password: %d", err);
                memzero(pin, sizeof(pin));
                return false;
            }
        }

        WalletError err = wallet_unlock(pin, strlen(pin));
        memzero(pin, sizeof(pin));
        if (err != WALLET_OK) {
            ESP_LOGE(TAG, "Failed to unlock wallet: %d", err);
            button_drain();
            return false;
        }
    }

    /* Key derivation blocks for roughly a second and a half - a PIN entry runs
     * PBKDF2 three times, once here in pin_verify() and twice inside
     * wallet_unlock() (see docs/AUDIT-SECRETS-2.md N1). Drop anything pressed
     * while the screen was frozen. */
    button_drain();
    return true;
}

/* ============================================================================
 * Helper: Parse mnemonic into word count
 * ============================================================================ */

static int count_mnemonic_words(const char *mnemonic)
{
    int count = 0;
    const char *p = mnemonic;

    while (*p) {
        /* Skip whitespace */
        while (*p == ' ') p++;
        if (*p == '\0') break;

        /* Count word */
        count++;

        /* Skip to next whitespace */
        while (*p && *p != ' ') p++;
    }

    return count;
}

/* ============================================================================
 * Helper: Get word N from mnemonic
 * ============================================================================ */

static bool get_mnemonic_word(const char *mnemonic, int index, char *word_out, size_t max_len)
{
    int count = 0;
    const char *p = mnemonic;

    while (*p) {
        /* Skip whitespace */
        while (*p == ' ') p++;
        if (*p == '\0') break;

        if (count == index) {
            /* Found the word - copy it */
            size_t i = 0;
            while (*p && *p != ' ' && i < max_len - 1) {
                word_out[i++] = *p++;
            }
            word_out[i] = '\0';
            return true;
        }

        /* Skip to next whitespace */
        while (*p && *p != ' ') p++;
        count++;
    }

    return false;
}

/* ============================================================================
 * Boot Screen
 * ============================================================================ */

static void screen_boot_enter(void)
{
    ESP_LOGI(TAG, "Boot screen (splash)");
    boot_splash_restart();
}

/* The splash, as its loop says it should look right now. Static buffers: the
 * tall picture is shown by pointer until the next clear. */
static void screen_boot_render(void)
{
    static uint8_t tall[SPLASH_TALL_BYTES];
    static uint8_t wide[SPLASH_WIDE_BYTES];

    boot_splash_shown = splash_loop_view(&boot_splash, ui_now_ms());
    oled_clear();
    if (boot_splash_shown.blank) {
        return;   /* all black: nothing lit to burn in, next to no current */
    }
    const char *prompt = boot_splash_shown.prompt ? HINT_PRESS_ANY : NULL;
    if (oled_panel_rows() >= 128) {
        splash_render_tall(boot_splash_shown.frame, prompt, tall);
        oled_show_panel_picture(tall);
    } else {
        splash_render_wide(boot_splash_shown.frame, prompt, wide);
        for (uint8_t page = 0; page < OLED_PAGES; page++) {
            oled_blit_page(page, wide + page * OLED_WIDTH, OLED_WIDTH);
        }
    }
}

static void screen_boot_on_button(button_id_t btn)
{
    switch (splash_loop_input(&boot_splash, ui_now_ms(), btn == BUTTON_ACCEPT, splash_ok_only)) {
        case SPLASH_INPUT_WOKE:
            ESP_LOGI(TAG, "Splash woken by %s", button_get_action_name(btn));
            ui_invalidate();
            return;
        case SPLASH_INPUT_PROMPT:
            ui_invalidate();
            return;
        case SPLASH_INPUT_DISMISS:
            break;
    }

    /* Initialize subsystems and determine next screen */
    pin_init();
    pending_mnemonic_display = false;  /* Clear any pending state */

    if (!pin_is_set()) {
        /* First boot - need to set up PIN */
        ui_set_screen(SCREEN_PIN_SETUP);
    } else if (pin_should_wipe()) {
        /* Attempts were exhausted but the wipe did not complete - most likely
         * power was cut between the two. Finish it now, before offering any
         * further attempts, otherwise a reboot resets the counter. */
        ESP_LOGW(TAG, "Resuming interrupted wipe");
        device_wipe();
        wallet_init();
        ui_set_screen(SCREEN_PIN_SETUP);
    } else {
        /* PIN is set - need to unlock */
        ui_set_screen(SCREEN_PIN_UNLOCK);
    }
}

/* ============================================================================
 * PIN Setup Screen
 * ============================================================================ */

static void screen_pin_setup_enter(void)
{
    ESP_LOGI(TAG, "PIN setup screen");
    pin_entry_reset();
    pin_confirm_mode = false;
    memset(pin_first_entry, 0, sizeof(pin_first_entry));
}

static void screen_pin_setup_render(void)
{
    oled_clear();

    if (!pin_confirm_mode) {
        oled_draw_string_centered(0, "Set New PIN");
    } else {
        oled_draw_string_centered(0, "Confirm PIN");
    }

    /* Draw the selector (digit, or OK once the PIN is long enough) */
    char digit_line[22];
    pin_option_label(digit_line, sizeof(digit_line));
    oled_draw_string_centered(2, digit_line);

    /* Draw PIN display with asterisks */
    char display[PIN_DISPLAY_LEN + 3];
    display[0] = '[';
    for (int i = 0; i < PIN_DISPLAY_LEN; i++) {
        if (i < pin_cursor) {
            display[i + 1] = '*';
        } else {
            display[i + 1] = '_';
        }
    }
    display[PIN_DISPLAY_LEN + 1] = ']';
    display[PIN_DISPLAY_LEN + 2] = '\0';
    oled_draw_string_centered(4, display);

    /* Show progress */
    char progress[22];
    if (pin_can_submit()) {
        snprintf(progress, sizeof(progress), "%d digits, pick OK", pin_cursor);
    } else {
        snprintf(progress, sizeof(progress), "%d of %d min", pin_cursor, PIN_MIN_LENGTH);
    }
    oled_draw_string_centered(5, progress);

    /* Draw instructions */
    oled_draw_string(7, 0, HINT_SCROLL_DEL_SEL);
}

static void screen_pin_setup_on_button(button_id_t btn)
{
    switch (btn) {
        case BUTTON_UP:
            pin_option_scroll(-1);   /* see PICKER_FORWARD */
            break;

        case BUTTON_DOWN:
            pin_option_scroll(1);
            break;

        case BUTTON_CANCEL:
            if (pin_cursor > 0) {
                pin_entry_backspace();
                current_digit = 0;
            }
            break;

        case BUTTON_ACCEPT:
            if (current_digit != PIN_OPTION_SUBMIT) {
                if (pin_cursor < PIN_MAX_LENGTH) {
                    pin_entry_add_digit(current_digit);
                }
                current_digit = 0;
                break;
            }

            /* Submit */
            if (pin_can_submit()) {
                current_digit = 0;
                if (!pin_confirm_mode) {
                    /* First entry - save and ask for confirmation */
                    strncpy(pin_first_entry, pin_entry, PIN_MAX_LENGTH);
                    pin_first_entry[PIN_MAX_LENGTH] = '\0';
                    pin_confirm_mode = true;
                    pin_entry_reset();
                } else {
                    /* Confirmation - check if matches */
                    if (strcmp(pin_first_entry, pin_entry) == 0) {
                        /* PINs match - set it */
                        if (pin_set(pin_entry)) {
                            ESP_LOGI(TAG, "PIN set successfully");
                            ui_set_screen(SCREEN_MAIN_MENU);
                        } else {
                            /* Failed to set - restart */
                            pin_entry_reset();
                            pin_confirm_mode = false;
                        }
                    } else {
                        /* PINs don't match - restart */
                        ESP_LOGW(TAG, "PINs don't match");
                        pin_entry_reset();
                        pin_confirm_mode = false;
                    }
                }
            }
            break;

        default:
            break;
    }

    ui_invalidate();
}

/* ============================================================================
 * PIN Unlock Screen
 * ============================================================================ */

static void screen_pin_unlock_enter(void)
{
    ESP_LOGI(TAG, "PIN unlock screen");
    pin_entry_reset();
    pin_idle_since_us = esp_timer_get_time();
}

static void screen_pin_unlock_render(void)
{
    oled_clear();
    oled_draw_string_centered(0, "Enter PIN");

    /* Draw the selector (digit, or OK once the PIN is long enough) */
    char digit_line[22];
    pin_option_label(digit_line, sizeof(digit_line));
    oled_draw_string_centered(2, digit_line);

    /* Draw PIN display with asterisks */
    char display[PIN_DISPLAY_LEN + 3];
    display[0] = '[';
    for (int i = 0; i < PIN_DISPLAY_LEN; i++) {
        if (i < pin_cursor) {
            display[i + 1] = '*';
        } else {
            display[i + 1] = '_';
        }
    }
    display[PIN_DISPLAY_LEN + 1] = ']';
    display[PIN_DISPLAY_LEN + 2] = '\0';
    oled_draw_string_centered(4, display);

    /* Attempts remaining, and how to submit once the PIN is long enough */
    char attempts_str[22];
    if (pin_can_submit()) {
        snprintf(attempts_str, sizeof(attempts_str), "Tries: %d - pick OK",
                 pin_get_remaining_attempts());
    } else {
        snprintf(attempts_str, sizeof(attempts_str), "Tries: %d",
                 pin_get_remaining_attempts());
    }
    oled_draw_string_centered(5, attempts_str);

    /* X deletes a digit; with none typed it is the way out. */
    oled_draw_string(7, 0, pin_cursor > 0 ? HINT_SCROLL_DEL_SEL : HINT_SCROLL_BACK_SEL);
}

static void screen_pin_unlock_on_button(button_id_t btn)
{
    switch (btn) {
        case BUTTON_UP:
            pin_option_scroll(-1);   /* see PICKER_FORWARD */
            break;

        case BUTTON_DOWN:
            pin_option_scroll(1);
            break;

        case BUTTON_CANCEL:
            if (pin_cursor > 0) {
                pin_entry_backspace();
                current_digit = 0;
                break;
            }
            /* Nothing to delete: leave, as the idle timeout does. The device
             * is locked either way, so this goes to the splash, never to the
             * menu; it also drops a pending Show Seed, which would otherwise
             * still be waiting behind the next ordinary unlock. No
             * pin_verify(), so the attempt counter is untouched. */
            pending_mnemonic_display = false;
            ui_set_screen(SCREEN_BOOT);
            return;

        case BUTTON_ACCEPT:
            if (current_digit != PIN_OPTION_SUBMIT) {
                if (pin_cursor < PIN_MAX_LENGTH) {
                    pin_entry_add_digit(current_digit);
                }
                current_digit = 0;
                break;
            }

            /* Submit - each press here costs one of the 3 attempts. */
            if (pin_can_submit()) {
                current_digit = 0;
                if (pin_verify(pin_entry)) {
                    ESP_LOGI(TAG, "PIN verified");

                    /* Unlock the vault now rather than lazily.
                     *
                     * Waiting until a screen needed keys produced a deadlock:
                     * the menu hid "View Address" because wallet_count was
                     * still zero, and wallet_count only became non-zero once
                     * something unlocked the vault - which only the hidden
                     * screen did. A rebooted device with wallets on it looked
                     * empty until the user wandered into Settings. */
                    ensure_wallet_unlocked();

                    if (pending_mnemonic_display) {
                        pending_mnemonic_display = false;
                        ui_set_screen(SCREEN_MNEMONIC_DISPLAY);
                    } else {
                        ui_set_screen(SCREEN_MAIN_MENU);
                    }
                } else {
                    /* Wrong PIN */
                    pin_entry_reset();

                    if (pin_should_wipe()) {
                        /* Wipe device and reset */
                        device_wipe();
                        ui_set_screen(SCREEN_PIN_SETUP);
                    }
                }
            }
            break;

        default:
            break;
    }

    ui_invalidate();
}

/* ============================================================================
 * PIN Change Screen
 *
 * Three entries: the current PIN, then the new one twice. The current PIN is
 * needed for more than authorisation - it derives the key every stored
 * mnemonic is encrypted under, and re-encrypting them needs both keys at once.
 * ============================================================================ */

typedef enum {
    PIN_CHANGE_CURRENT = 0,
    PIN_CHANGE_NEW,
    PIN_CHANGE_CONFIRM,
} PinChangePhase;

static PinChangePhase pin_change_phase = PIN_CHANGE_CURRENT;
static char pin_change_current[PIN_MAX_LENGTH + 1] = {0};
static char pin_change_message[22] = {0};

static void pin_change_clear_secrets(void)
{
    memzero(pin_change_current, sizeof(pin_change_current));
    memzero(pin_first_entry, sizeof(pin_first_entry));
    pin_entry_reset();
}

/* Re-encryption is seconds of PBKDF2 and AES with no button polling in
 * between, so without this the device looks hung at exactly the moment the
 * user must not pull the power. */
static void pin_change_progress_cb(uint8_t done, uint8_t total)
{
    oled_clear();
    oled_draw_string_centered(1, "Changing PIN");
    oled_draw_string_centered(3, "Re-encrypting...");

    char line[22];
    snprintf(line, sizeof(line), "%d of %d", (int)done, (int)total);
    oled_draw_string_centered(5, line);
    oled_draw_string_centered(7, "Do not power off");
    oled_flush();
}

static void screen_pin_change_enter(void)
{
    ESP_LOGI(TAG, "PIN change screen");
    pin_change_phase = PIN_CHANGE_CURRENT;
    pin_change_message[0] = '\0';
    pin_change_clear_secrets();
}

static void screen_pin_change_exit(screen_id_t next)
{
    (void)next;
    pin_change_clear_secrets();
    pin_set_change_progress(NULL);
}

static void screen_pin_change_render(void)
{
    oled_clear();

    switch (pin_change_phase) {
        case PIN_CHANGE_CURRENT: oled_draw_string_centered(0, "Current PIN"); break;
        case PIN_CHANGE_NEW:     oled_draw_string_centered(0, "New PIN");     break;
        case PIN_CHANGE_CONFIRM: oled_draw_string_centered(0, "Confirm New PIN"); break;
    }

    char digit_line[22];
    pin_option_label(digit_line, sizeof(digit_line));
    oled_draw_string_centered(2, digit_line);

    char display[PIN_DISPLAY_LEN + 3];
    display[0] = '[';
    for (int i = 0; i < PIN_DISPLAY_LEN; i++) {
        display[i + 1] = (i < pin_cursor) ? '*' : '_';
    }
    display[PIN_DISPLAY_LEN + 1] = ']';
    display[PIN_DISPLAY_LEN + 2] = '\0';
    oled_draw_string_centered(4, display);

    if (pin_change_message[0] != '\0') {
        oled_draw_string_centered(5, pin_change_message);
    } else if (pin_can_submit()) {
        oled_draw_string_centered(5, "Pick OK when done");
    } else {
        char progress[22];
        snprintf(progress, sizeof(progress), "%d of %d min", pin_cursor, PIN_MIN_LENGTH);
        oled_draw_string_centered(5, progress);
    }

    oled_draw_string(7, 0, HINT_SCROLL_DEL_SEL);
}

/* The whole change, once all three PINs are in hand. */
static void pin_change_commit(void)
{
    pin_set_change_progress(pin_change_progress_cb);
    bool ok = pin_change(pin_change_current, pin_first_entry);
    pin_set_change_progress(NULL);

    if (ok) {
        ESP_LOGI(TAG, "PIN changed");
        pin_change_clear_secrets();
        ui_set_screen(SCREEN_SETTINGS);
        return;
    }

    /* Say which failure it was. "Wrong PIN" and "a wallet would not decrypt"
     * call for completely different reactions from the user, and a device that
     * refuses without explaining leaves them retyping a PIN that was right.
     * Asking the vault afterwards is safe: it costs no attempt, and the answer
     * is one the user just proved they are entitled to. */
    if (wallet_verify_password(pin_change_current, strlen(pin_change_current))) {
        strncpy(pin_change_message, "Wallet unreadable", sizeof(pin_change_message) - 1);
        ESP_LOGE(TAG, "PIN change refused: a wallet failed verification");
    } else {
        strncpy(pin_change_message, "Wrong current PIN", sizeof(pin_change_message) - 1);
    }
    pin_change_message[sizeof(pin_change_message) - 1] = '\0';

    /* Nothing was written, so start over rather than leaving half the entries
     * standing. */
    pin_change_phase = PIN_CHANGE_CURRENT;
    pin_change_clear_secrets();

    if (pin_should_wipe()) {
        device_wipe();
        ui_set_screen(SCREEN_PIN_SETUP);
    }
}

static void screen_pin_change_on_button(button_id_t btn)
{
    switch (btn) {
        case BUTTON_UP:
            pin_option_scroll(-1);   /* see PICKER_FORWARD */
            break;

        case BUTTON_DOWN:
            pin_option_scroll(1);
            break;

        case BUTTON_CANCEL:
            if (pin_cursor > 0) {
                pin_entry_backspace();
                current_digit = 0;
            } else {
                pin_change_clear_secrets();
                ui_set_screen(SCREEN_SETTINGS);
                return;
            }
            break;

        case BUTTON_ACCEPT:
            if (current_digit != PIN_OPTION_SUBMIT) {
                if (pin_cursor < PIN_MAX_LENGTH) {
                    pin_entry_add_digit(current_digit);
                }
                current_digit = 0;
                break;
            }

            if (!pin_can_submit()) {
                break;
            }
            current_digit = 0;
            pin_change_message[0] = '\0';

            if (pin_change_phase == PIN_CHANGE_CURRENT) {
                strncpy(pin_change_current, pin_entry, PIN_MAX_LENGTH);
                pin_change_current[PIN_MAX_LENGTH] = '\0';
                pin_entry_reset();
                pin_change_phase = PIN_CHANGE_NEW;
            } else if (pin_change_phase == PIN_CHANGE_NEW) {
                strncpy(pin_first_entry, pin_entry, PIN_MAX_LENGTH);
                pin_first_entry[PIN_MAX_LENGTH] = '\0';
                pin_entry_reset();
                pin_change_phase = PIN_CHANGE_CONFIRM;
            } else {
                if (strcmp(pin_first_entry, pin_entry) != 0) {
                    strncpy(pin_change_message, "PINs don't match",
                            sizeof(pin_change_message) - 1);
                    pin_entry_reset();
                    memzero(pin_first_entry, sizeof(pin_first_entry));
                    pin_change_phase = PIN_CHANGE_NEW;
                    break;
                }
                pin_change_commit();
                return;
            }
            break;

        default:
            break;
    }

    ui_invalidate();
}

/* ============================================================================
 * Main Menu Screen
 * ============================================================================ */

/* Roadmap 1.5: the home screen names the open seed, so the first thing seen
 * after unlocking (or after a passphrase, a wallet switch or a temporary
 * seed) is which wallet this is. Derived here, on entry, not per repaint: the
 * first call after an unlock pays for the seed. */
static void home_fingerprint_refresh(void)
{
    WalletStatus status = wallet_get_status();
    const bool temp = wallet_has_temporary_mnemonic();
    if (!pin_is_unlocked() || (status.wallet_count == 0 && !temp)) {
        return;
    }
    if (!ensure_wallet_unlocked()) {
        return;
    }
    /* As the wallet screen does, and with the same exception: never over a
     * temporary seed. */
    if (!temp && wallet_get_status().active_wallet_index == 0 &&
        wallet_select_wallet(1) != WALLET_OK) {
        return;
    }
    wallet_info_refresh();
}

static void screen_main_menu_enter(void)
{
    /* A no-op unless seed creation suspended them. Every exit from that flow
     * arrives here, which is why the resume lives at the destination rather
     * than being repeated at each of the ways out. */
    transport_resume();

    ESP_LOGI(TAG, "Main menu screen");
    menu_selection = 0;
    menu_rebuild();
    home_fingerprint_refresh();
}

static void screen_main_menu_render(void)
{
    oled_clear();
    menu_rebuild();

    /* Show which wallet is active, so "Wallet Info" is not a mystery box. */
    WalletStatus status = wallet_get_status();
    char header[40];
    char who[16] = "Menu";
    if (wallet_has_temporary_mnemonic()) {
        /* Named on the home screen, permanently, in place of the wallet
         * number. A user who forgets which mode the device is in and reboots
         * has lost the seed; there is no recovering it from anywhere. */
        snprintf(who, sizeof(who), "TEMP");
    } else if (status.wallet_count > 1) {
        snprintf(who, sizeof(who), "W%u/%u",
                 (unsigned)status.active_wallet_index, (unsigned)status.wallet_count);
    }
    if (master_zfp[0]) {
        /* Roadmap 1.5: which seed is open, first thing after unlocking. */
        snprintf(header, sizeof(header), "%s  ZFP %s", who, master_zfp);
    } else {
        snprintf(header, sizeof(header), "-- Menu --%s%s",
                 strcmp(who, "Menu") ? " " : "", strcmp(who, "Menu") ? who : "");
    }
    header[21] = '\0';   /* the panel's width; nothing here reaches it */
    oled_draw_string_centered(0, header);

    /* Draw menu items (3 visible at a time on 128x64) */
    int start = (menu_selection > 1) ? menu_selection - 1 : 0;
    if (start > menu_item_count - 3) {
        start = menu_item_count - 3;
    }
    if (start < 0) start = 0;

    for (int i = 0; i < 3 && (start + i) < menu_item_count; i++) {
        int item_idx = start + i;
        char line[22];

        snprintf(line, sizeof(line), "%s %s",
                 item_idx == menu_selection ? ">" : " ",
                 menu_action_label(menu_actions[item_idx]));
        oled_draw_string(2 + i * 2, 0, line);
    }

    /* Draw instructions */
    oled_draw_string(7, 0, HINT_HOME);
}

static void screen_main_menu_on_button(button_id_t btn)
{
    switch (btn) {
        case BUTTON_UP:
            if (menu_selection > 0) {
                menu_selection--;
            }
            break;

        case BUTTON_DOWN:
            if (menu_selection < menu_item_count - 1) {
                menu_selection++;
            }
            break;

        case BUTTON_ACCEPT:
            if (menu_selection < 0 || menu_selection >= menu_item_count) {
                break;
            }
            switch (menu_actions[menu_selection]) {
                case MENU_VIEW_ADDRESS:
                    ui_set_screen(SCREEN_WALLET_INFO);
                    break;
                case MENU_SELECT_WALLET:
                    ui_set_screen(SCREEN_WALLET_SELECT);
                    break;
                case MENU_NEW_WALLET:
                    /* Length first: the entropy gate is 128 bits for 12 words
                     * and 256 for 24, so the screen that collects it has to
                     * know which was asked for. */
                    create_entropy_ready = false;
                    ui_set_screen(SCREEN_WALLET_CREATE);
                    break;
                case MENU_IMPORT_WALLET:
                    entry_is_temporary = false;
                    ui_set_screen(SCREEN_MNEMONIC_ENTRY);
                    break;
                case MENU_TEMP_SEED:
                    ui_set_screen(SCREEN_TEMP_SEED);
                    break;
                case MENU_SD_SEED:
                    ui_set_screen(SCREEN_SD_SEED);
                    break;
                case MENU_END_TEMP:
                    /* Ending the mode is locking. There is no lighter version:
                     * the seed only exists in RAM, so "put it away" and "destroy
                     * it" are the same operation. */
                    lock_device();
                    ui_set_screen(SCREEN_PIN_UNLOCK);
                    break;
                case MENU_SHOW_ADDRESS:
                    ui_set_screen(SCREEN_SHOW_ADDRESS);
                    break;
                case MENU_SCAN:
                    ui_set_screen(SCREEN_SCAN);
                    break;
                case MENU_SETTINGS:
                    ui_set_screen(SCREEN_SETTINGS);
                    break;
                default:
                    break;
            }
            break;

        case BUTTON_CANCEL:
            /* Lock device -- held, not tapped. See LOCK_HOLD_US.
             *
             * The lock itself called pin_lock() alone and shipped that way:
             * the PIN gate closed and the vault stayed open underneath it, so
             * unlocking again landed the user back in their passphrase wallet
             * with the seed never having left RAM. Reported from hardware. It
             * is the deliberate lock, so it must be at least as strong as the
             * one that happens by itself -- which is why it goes through
             * lock_device() and not through anything lighter. */
            ui_set_screen(SCREEN_LOCK_HOLD);
            break;

        default:
            break;
    }

    ui_invalidate();
}

/* ============================================================================
 * Wallet Info Screen
 *
 * Which seed is active, whether a passphrase is on it, and its fingerprint.
 * LeekWallet put an address here; there is no Zcash address to show
 * until a later phase, so the screen says that instead of drawing nothing.
 * ============================================================================ */

/* ------------------------------------------------------------ XFP (T39b)
 *
 * The BIP32 master fingerprint: eight hex characters that identify the *seed*
 * rather than one address off it. That is what makes it worth the row it costs
 * on a 128x64 screen - a wrong passphrase does not fail, it derives a
 * different, perfectly valid, empty-looking wallet, and the fingerprint is the
 * cheapest way to see that has happened. It is the same value Electrum, Trezor
 * Suite and every PSBT show, so it can be checked against something.
 *
 * Not a secret: it is published in every watch-only descriptor.
 *
 * Cached rather than derived per render, because deriving it runs PBKDF2 on a
 * cold seed cache and the render path runs on every repaint. */
static void refresh_master_xfp(void)
{
    uint32_t fp = 0;

    master_xfp[0] = '\0';
    master_zfp[0] = '\0';
    if (wallet_get_master_fingerprint(&fp) == WALLET_OK) {
        snprintf(master_xfp, sizeof(master_xfp), "%08lX", (unsigned long)fp);
    }
#if LEEK_HAS_ZCASH
    uint8_t zfp[32];
    if (zcash_seed_fingerprint(zfp) == WALLET_OK) {
        zcash_short_fp(zfp, master_zfp);
    }
    memzero(zfp, sizeof zfp);
#endif
}

/* Draw the fingerprint, or nothing at all. A blank row says "not shown"; a row
 * reading "XFP" with a placeholder after it would be read as a value. */
static void draw_master_xfp(int row, const char *prefix)
{
    if (master_xfp[0] == '\0') {
        return;
    }

    char line[22];
    snprintf(line, sizeof(line), "%s %s", prefix, master_xfp);
    oled_draw_string_centered(row, line);
}

/* The Zcash seed fingerprint, under the same rule: the value or nothing. */
static void draw_master_zfp(int row)
{
    if (master_zfp[0] == '\0') {
        return;
    }

    char line[22];
    snprintf(line, sizeof(line), "ZFP %s", master_zfp);
    oled_draw_string_centered(row, line);
}

/* Why the screen has nothing to show, or "" when it has a fingerprint. Kept
 * apart from master_xfp so an error can never be drawn where an identifier
 * goes (AUDIT S8a, T7). */
static char wallet_info_error[24] = {0};

/* Re-read the fingerprint and the passphrase state it was read under. Always
 * together: an XFP from before a passphrase change beside a marker from after
 * it would be worse than showing no fingerprint at all. */
static void wallet_info_refresh(void)
{
    refresh_master_xfp();
    wallet_info_passphrase_shown = wallet_has_passphrase();
}

static void screen_wallet_info_enter(void)
{
    ESP_LOGI(TAG, "Wallet info screen");

    wallet_info_error[0] = '\0';
    master_xfp[0] = '\0';
    master_zfp[0] = '\0';

    if (!ensure_wallet_unlocked()) {
        snprintf(wallet_info_error, sizeof(wallet_info_error), "Unlock failed");
        return;
    }

    WalletStatus status = wallet_get_status();
    const bool temp = wallet_has_temporary_mnemonic();
    if (status.wallet_count == 0 && !temp) {
        snprintf(wallet_info_error, sizeof(wallet_info_error), "No wallet");
        return;
    }

    /* Select first wallet if none active.
     *
     * Not in temporary mode: the active index is 0 there by design, and
     * "helpfully" selecting wallet 1 would load a stored seed over the one the
     * user typed - silently showing the wrong wallet, which is the exact
     * failure this screen's title row exists to make impossible. */
    if (!temp && status.active_wallet_index == 0) {
        WalletError err = wallet_select_wallet(1);
        if (err != WALLET_OK) {
            ESP_LOGE(TAG, "Failed to select wallet: %d", err);
            snprintf(wallet_info_error, sizeof(wallet_info_error), "Select failed");
            return;
        }
    }

    wallet_info_refresh();
}

static void screen_wallet_info_render(void)
{
    oled_clear();

    WalletStatus status = wallet_get_status();
    const bool temp = wallet_has_temporary_mnemonic();
    char title[22];
    /* Which seed, and whether a passphrase is on it. The passphrase marker is
     * on the title row rather than buried because a passphrase changes the
     * wallet and leaves no other visible trace (T42) - the XFP below says the
     * seed changed, but only to someone who recorded it. */
    if (temp) {
        /* "TEMP" where the wallet number goes, because there is no wallet
         * number: this seed is held by no slot. */
        snprintf(title, sizeof(title), "TEMP  acct %u%s",
                 (unsigned)hd_account, wallet_has_passphrase() ? " P" : "");
    } else {
        snprintf(title, sizeof(title), "W%u/%u  acct %u%s",
                 (unsigned)status.active_wallet_index, (unsigned)status.wallet_count,
                 (unsigned)hd_account, wallet_has_passphrase() ? " P" : "");
    }
    oled_draw_string_centered(0, title);

    if (status.wallet_count == 0 && !temp) {
        oled_draw_string_centered(3, "No wallet");
        oled_draw_string_centered(4, "Create one first");
    } else if (wallet_info_error[0] || master_xfp[0] == '\0') {
        /* Say plainly that there is nothing to show, rather than drawing
         * something identifier-shaped. */
        oled_draw_string_centered(2, "Error");
        oled_draw_string_centered(4, wallet_info_error[0] ? wallet_info_error
                                                          : "No fingerprint");
    } else {
        /* Which seed this is. The fingerprint is the one identifier that
         * does not depend on a chain, and the one a wrong passphrase changes. */
        draw_master_xfp(2, "XFP");
        draw_master_zfp(4);
    }

    oled_draw_string(7, 0, HINT_BACK);
}

static void screen_wallet_info_on_button(button_id_t btn)
{
    if (btn == BUTTON_CANCEL) {
        ui_set_screen(SCREEN_MAIN_MENU);
    }
    ui_invalidate();
}

/* ============================================================================
 * Lock: held, not tapped
 * ============================================================================ */

/* LOCK_HOLD_US, and why locking is held rather than tapped, are in ui.h.
 *
 * Width of the bar, in characters. Text rather than pixels because every other
 * screen here is text, and because a test can read it back. */
#define LOCK_HOLD_SEGMENTS 16

static int64_t lock_hold_start_us = 0;

/* Is there anything here that a lock would cost more than a PIN entry to get
 * back? Both of these live only in RAM and both die in wallet_lock(). */
static bool lock_would_lose_something(void)
{
    return wallet_has_passphrase() || wallet_has_temporary_mnemonic();
}

static void screen_lock_hold_enter(void)
{
    ESP_LOGI(TAG, "Lock hold screen");
    lock_hold_start_us = esp_timer_get_time();
}

static void screen_lock_hold_render(void)
{
    int64_t held = esp_timer_get_time() - lock_hold_start_us;
    if (held < 0) held = 0;

    int filled = (int)((held * LOCK_HOLD_SEGMENTS) / LOCK_HOLD_US);
    if (filled > LOCK_HOLD_SEGMENTS) filled = LOCK_HOLD_SEGMENTS;

    char bar[LOCK_HOLD_SEGMENTS + 3];
    bar[0] = '[';
    for (int i = 0; i < LOCK_HOLD_SEGMENTS; i++) {
        bar[i + 1] = (i < filled) ? '#' : ' ';
    }
    bar[LOCK_HOLD_SEGMENTS + 1] = ']';
    bar[LOCK_HOLD_SEGMENTS + 2] = '\0';

    oled_clear();
    oled_draw_string_centered(0, "Lock device");
    oled_draw_string_centered(2, HINT_KEEP_HOLDING);
    oled_draw_string_centered(4, bar);
    oled_draw_string_centered(6, "Let go to cancel");
}

static void screen_lock_hold_on_button(button_id_t btn)
{
    /* Any other button abandons it. The hold itself is not a button event --
     * there is no press to repeat and no release to hear -- so it is watched
     * from the task loop instead. See service_lock_hold(). */
    (void)btn;
    ui_set_screen(SCREEN_MAIN_MENU);
}

/**
 * Watch the hold. Called once per turn of the UI task's loop.
 *
 * Polls the debounced level rather than waiting for an event, because a button
 * held down produces exactly one event and no more, and a button released
 * produces none at all.
 */
static void service_lock_hold(void)
{
    if (ui_get_screen() != SCREEN_LOCK_HOLD) {
        return;
    }

    if (!button_is_pressed(BUTTON_CANCEL)) {
        /* A tap. Which is what leaving a menu looks like, and is exactly why
         * this screen exists. */
        ui_set_screen(SCREEN_MAIN_MENU);
        return;
    }

    if (esp_timer_get_time() - lock_hold_start_us < LOCK_HOLD_US) {
        ui_invalidate();        /* advance the bar */
        return;
    }

    if (lock_would_lose_something()) {
        ui_set_screen(SCREEN_LOCK_CONFIRM);
        return;
    }

    ESP_LOGI(TAG, "Locked by held BACK");
    lock_device();
    ui_set_screen(SCREEN_PIN_UNLOCK);
}

/* Names what the lock costs, rather than asking whether the user is sure.
 *
 * "Are you sure?" is answered reflexively within a week and then protects
 * nothing. What the user cannot see from the home screen is which of the two
 * RAM-only secrets is about to go, and how much typing it will take to get it
 * back -- so that is what this says. */
static void screen_lock_confirm_render(void)
{
    bool temp = wallet_has_temporary_mnemonic();
    bool pass = wallet_has_passphrase();

    oled_clear();
    oled_draw_string_centered(0, "Lock device?");

    if (temp && pass) {
        oled_draw_string_centered(2, "Temp seed AND");
        oled_draw_string_centered(3, "passphrase go.");
        oled_draw_string_centered(5, "Type both again.");
    } else if (temp) {
        oled_draw_string_centered(2, "Temp seed goes.");
        oled_draw_string_centered(4, "Nothing stored it.");
        oled_draw_string_centered(5, "Type it again.");
    } else {
        oled_draw_string_centered(2, "Passphrase goes.");
        oled_draw_string_centered(4, "Type it again to");
        oled_draw_string_centered(5, "reach this wallet.");
    }

    oled_draw_string(7, 0, HINT_LOCK_CONFIRM);
}

static void screen_lock_confirm_on_button(button_id_t btn)
{
    if (btn == BUTTON_ACCEPT) {
        ESP_LOGI(TAG, "Locked by held BACK, confirmed");
        lock_device();
        ui_set_screen(SCREEN_PIN_UNLOCK);
        return;
    }
    if (btn == BUTTON_CANCEL) {
        ui_set_screen(SCREEN_MAIN_MENU);
    }
}

/* Settings -> Show Seed. The lock that follows is deliberate (see
 * SET_SHOW_SEED), but it was a trap: the PIN screen had no way back, so
 * someone who opened Show Seed by mistake, or handed the board to a friend
 * who did, could only type the PIN or wait. This says what happens before it
 * happens, and BACK leaves the device unlocked on Settings. */
static void screen_show_seed_confirm_render(void)
{
    oled_clear();
    oled_draw_string_centered(0, "Show seed?");
    oled_draw_string_centered(2, "This locks the");
    oled_draw_string_centered(3, "device. Enter your");
    oled_draw_string_centered(4, "PIN to see words.");
    oled_draw_string(7, 0, HINT_SHOW_SEED);
}

static void screen_show_seed_confirm_on_button(button_id_t btn)
{
    if (btn == BUTTON_ACCEPT) {
        ESP_LOGI(TAG, "Show Seed: locking to re-ask the PIN");
        lock_device();
        pending_mnemonic_display = true;
        ui_set_screen(SCREEN_PIN_UNLOCK);
        return;
    }
    if (btn == BUTTON_CANCEL) {
        ui_set_screen(SCREEN_SETTINGS);
    }
}

/* ============================================================================
 * Wallet Create Screen
 * ============================================================================ */

static void screen_wallet_create_enter(void)
{
    ESP_LOGI(TAG, "Wallet create screen");
    create_show_mnemonic = false;
    create_generate_pending = false;
    create_error[0] = '\0';
    memset(mnemonic_buffer, 0, sizeof(mnemonic_buffer));

    if (create_entropy_ready) {
        /* Coming back from the entropy screen with the gate met for the length
         * chosen on the way in. Generate straight away: asking again would
         * invite a different answer, and the entropy that was just collected
         * was measured against the first one. */
        create_entropy_ready = false;
        create_show_mnemonic = true;
        create_generate_pending = true;
        return;
    }

    /* Fresh start. The length is chosen here, before any entropy is gathered,
     * because the gate depends on it. */
    create_word_count = 12;
}

static void screen_wallet_create_render(void)
{
    oled_clear();

    if (!create_show_mnemonic) {
        oled_draw_string_centered(0, "New Wallet");

        char words_str[16];
        snprintf(words_str, sizeof(words_str), "< %d words >", create_word_count);
        oled_draw_string_centered(2, words_str);

        oled_draw_string_centered(4, HINT_CREATE_LENGTH);

        if (create_error[0] != '\0') {
            oled_draw_string_centered(5, create_error);
        }

        oled_draw_string(7, 0, HINT_CREATE);
    } else {
        /* Show "generating" message briefly, then switch to mnemonic display */
        oled_draw_string_centered(3, "Generating...");
    }
}

static void screen_wallet_create_on_button(button_id_t btn)
{
    switch (btn) {
        case BUTTON_UP:
        case BUTTON_DOWN:
            /* Toggle between 12 and 24 words */
            create_word_count = (create_word_count == 12) ? 24 : 12;
            break;

        case BUTTON_CANCEL:
            ui_set_screen(SCREEN_MAIN_MENU);
            break;

        case BUTTON_ACCEPT:
            if (!create_show_mnemonic) {
                create_error[0] = '\0';
                /* Length first, then entropy measured against it. The screen
                 * that gathers it reads create_word_count for its target and
                 * returns here to generate. */
                ui_set_screen(SCREEN_ENTROPY);
            }
            break;

        default:
            break;
    }

    ui_invalidate();
}

/* The slow half of "GEN", run from ui_poll_deferred() with the
 * "Generating..." frame already on the panel (AUDIT S8f).
 *
 * Every exit path clears create_generate_pending, including the failures: a
 * flag that survives its own handler would regenerate a seed on the next pass
 * through the loop, silently, and the second one would be the wallet the user
 * ends up with. */
static void wallet_create_run_generation(void)
{
    create_generate_pending = false;

    if (!ensure_wallet_unlocked()) {
        ESP_LOGE(TAG, "Failed to unlock wallet");
        snprintf(create_error, sizeof(create_error), "Unlock failed");
        create_show_mnemonic = false;
        ui_invalidate();
        return;
    }

    WalletError err = wallet_create_mnemonic(create_word_count,
                                             mnemonic_buffer,
                                             sizeof(mnemonic_buffer));
    if (err != WALLET_OK) {
        ESP_LOGE(TAG, "Failed to create mnemonic: %d", err);
        snprintf(create_error, sizeof(create_error), "Gen err: %d", err);
        create_show_mnemonic = false;
        ui_invalidate();
        return;
    }

    /* wallet_create_mnemonic already stores and selects the wallet. A new seed
     * is a new set of identities, so the account selection goes back to 0 for
     * the same reason a wallet switch resets it (T45). */
    hd_account_set(0);

    WalletStatus status = wallet_get_status();
    ESP_LOGI(TAG, "Created wallet %d with %d words",
             status.active_wallet_index, create_word_count);

    /* Presses made during the generation belong to the screen the user was
     * looking at, not the one they are about to get. */
    button_drain();
    ui_set_screen(SCREEN_MNEMONIC_DISPLAY);
    ui_invalidate();
}

/* ============================================================================
 * Wallet Select Screen
 * ============================================================================ */

static void screen_wallet_select_enter(void)
{
    ESP_LOGI(TAG, "Wallet select screen");
    WalletStatus status = wallet_get_status();
    wallet_list_selection = (status.active_wallet_index > 0) ?
                            status.active_wallet_index - 1 : 0;
}

static void screen_wallet_select_render(void)
{
    oled_clear();
    oled_draw_string_centered(0, "Select Wallet");

    WalletStatus status = wallet_get_status();

    if (status.wallet_count == 0) {
        oled_draw_string_centered(3, "No wallets");
        oled_draw_string_centered(4, "Create one first");
    } else {
        /* List wallets (3 visible at a time) */
        int start = (wallet_list_selection > 1) ? wallet_list_selection - 1 : 0;
        if (start > (int)status.wallet_count - 3) {
            start = status.wallet_count - 3;
        }
        if (start < 0) start = 0;

        for (int i = 0; i < 3 && (start + i) < status.wallet_count; i++) {
            int idx = start + i;
            char line[22];

            if (idx == wallet_list_selection) {
                if (idx + 1 == status.active_wallet_index) {
                    snprintf(line, sizeof(line), "> Wallet %d *", idx + 1);
                } else {
                    snprintf(line, sizeof(line), "> Wallet %d", idx + 1);
                }
            } else {
                if (idx + 1 == status.active_wallet_index) {
                    snprintf(line, sizeof(line), "  Wallet %d *", idx + 1);
                } else {
                    snprintf(line, sizeof(line), "  Wallet %d", idx + 1);
                }
            }
            oled_draw_string(2 + i * 2, 0, line);
        }
    }

    oled_draw_string(7, 0, HINT_WALLET_SELECT);
}

static void screen_wallet_select_on_button(button_id_t btn)
{
    WalletStatus status = wallet_get_status();

    switch (btn) {
        case BUTTON_UP:
            if (wallet_list_selection > 0) {
                wallet_list_selection--;
            }
            break;

        case BUTTON_DOWN:
            if (wallet_list_selection < (int)status.wallet_count - 1) {
                wallet_list_selection++;
            }
            break;

        case BUTTON_CANCEL:
            ui_set_screen(SCREEN_MAIN_MENU);
            break;

        case BUTTON_ACCEPT:
            if (status.wallet_count > 0) {
                wallet_select_wallet(wallet_list_selection + 1);
                /* The ACCOUNT is deliberately kept. Every seed has an account
                 * 3, so unlike the passphrase there is nothing seed-specific
                 * being carried across - and the wallet screen names the full
                 * path, so the selection is visible rather than implied. What
                 * a passphrase carries across a switch is a wallet nobody
                 * asked for; what an account number carries is a number. */
                ESP_LOGI(TAG, "Selected wallet %d", wallet_list_selection + 1);
                ui_set_screen(SCREEN_WALLET_INFO);
            }
            break;

        default:
            break;
    }

    ui_invalidate();
}

/* ============================================================================
 * Mnemonic Display Screen
 * ============================================================================ */

static void screen_mnemonic_display_enter(void)
{
    ESP_LOGI(TAG, "Mnemonic display screen");
    mnemonic_page = 0;

    if (!ensure_wallet_unlocked()) {
        return;
    }

    /* Get mnemonic for current wallet */
    WalletError err = wallet_get_mnemonic(mnemonic_buffer, sizeof(mnemonic_buffer));
    if (err != WALLET_OK) {
        ESP_LOGE(TAG, "Failed to get mnemonic: %d", err);
        strcpy(mnemonic_buffer, "");
        mnemonic_word_count = 0;
        return;
    }

    mnemonic_word_count = count_mnemonic_words(mnemonic_buffer);
    ESP_LOGI(TAG, "Displaying %d words", mnemonic_word_count);
}

static void screen_mnemonic_display_render(void)
{
    oled_clear();

    if (mnemonic_word_count == 0) {
        oled_draw_string_centered(3, "No mnemonic");
        oled_draw_string(7, 0, HINT_BACK);
        return;
    }

    /* Name the wallet and the page.
     *
     * With more than one seed stored, "Seed Phrase" alone is how a backup ends
     * up labelled with the wrong wallet - and a phrase written down under the
     * wrong name is a phrase you will not find when you need it. */
    int total_pages = (mnemonic_word_count + 2) / 3;
    WalletStatus mstatus = wallet_get_status();
    /* Clamped so the compiler can size the buffer: a 24-word phrase is 8 pages
     * and MAX_WALLETS is 30, so none of these need more than two digits. */
    unsigned page_no  = (unsigned)(mnemonic_page + 1) & 0xF;
    unsigned page_tot = (unsigned)total_pages & 0xF;
    char mheader[22];
    if (mstatus.wallet_count > 1) {
        snprintf(mheader, sizeof(mheader), "Seed W%u/%u %u-%u",
                 (unsigned)mstatus.active_wallet_index & 0x3F,
                 (unsigned)mstatus.wallet_count & 0x3F,
                 page_no, page_tot);
    } else {
        snprintf(mheader, sizeof(mheader), "Seed Phrase %u/%u", page_no, page_tot);
    }
    oled_draw_string_centered(0, mheader);

    /* Display 3 words */
    for (int i = 0; i < 3; i++) {
        int word_idx = mnemonic_page * 3 + i;
        if (word_idx >= mnemonic_word_count) break;

        char word[10];
        if (get_mnemonic_word(mnemonic_buffer, word_idx, word, sizeof(word))) {
            char line[24];
            int n = word_idx + 1;
            snprintf(line, sizeof(line), "%d. %s", n, word);
            oled_draw_string(2 + i, 0, line);
        }
    }

    /* Navigation hint */
    bool has_prev = mnemonic_page > 0;
    bool has_next = mnemonic_page < total_pages - 1;

    if (has_prev && has_next) {
        oled_draw_string(7, 0, HINT_SEED_MIDDLE);
    } else if (has_prev) {
        oled_draw_string(7, 0, HINT_SEED_LAST);
    } else if (has_next) {
        oled_draw_string(7, 0, HINT_SEED_FIRST);
    } else {
        oled_draw_string(7, 0, HINT_SEED_ONLY);
    }
}

static void screen_mnemonic_display_on_button(button_id_t btn)
{
    int total_pages = (mnemonic_word_count + 2) / 3;

    switch (btn) {
        case BUTTON_UP:
        case BUTTON_CANCEL:
            /* Previous page or back */
            if (mnemonic_page > 0) {
                mnemonic_page--;
            } else {
                ui_set_screen(SCREEN_WALLET_INFO);
            }
            break;

        case BUTTON_DOWN:
        case BUTTON_ACCEPT:
            /* Next page, or on the last page move to verification */
            if (mnemonic_page < total_pages - 1) {
                mnemonic_page++;
            } else if (mnemonic_word_count > 0) {
                ui_set_screen(SCREEN_MNEMONIC_VERIFY);
            } else {
                ui_set_screen(SCREEN_WALLET_INFO);
            }
            break;

        default:
            break;
    }

    ui_invalidate();
}

/* ============================================================================
 * Mnemonic Entry Screen (Import Wallet)
 * ============================================================================ */

static void screen_mnemonic_entry_enter(void)
{
    ESP_LOGI(TAG, "Mnemonic entry screen");
    entry_choosing_length = true;
    entry_length_choice = 12;
    mnemonic_entry_reset(&entry, entry_length_choice);
    entry_error[0] = '\0';
}

/*
 * One line describing what ACCEPT would do right now, shared by the import and
 * verify screens so the two can never drift into showing different things.
 *
 * The selector has three shapes: a block of letters ("ord[a-f]"), a single
 * letter ("ord[i]"), and - once few enough words still match - the whole word,
 * which is shown in full because that is what a press would commit.
 */
static void entry_selector_line(char *out, size_t len)
{
    const char *word = mnemonic_entry_selected_word(&entry);
    if (word) {
        snprintf(out, len, "OK:%s", word);
        return;
    }

    char label[MNEMONIC_ENTRY_WORD_LEN + 8];
    mnemonic_entry_option_label(&entry, label, sizeof(label));

    if (mnemonic_entry_option(&entry) == MNEMONIC_ENTRY_COMMIT &&
        !mnemonic_entry_on_group(&entry)) {
        /* Name the word OK would accept. "pos[OK]" tells the user nothing;
         * "OK:position" lets them catch a wrong turn before committing it. */
        const char *target = mnemonic_entry_suggestion(&entry);
        snprintf(out, len, "OK:%s", target ? target : entry.prefix);
        return;
    }

    snprintf(out, len, "%s[%s]", entry.prefix, label);
}

static void screen_mnemonic_entry_render(void)
{
    oled_clear();

    if (entry_choosing_length) {
        oled_draw_string_centered(0, entry_is_temporary ? "Temp seed" : "Import seed");
        oled_draw_string(2, 0, "How many words?");
        if (entry_is_temporary) {
            /* Repeated here, one screen after the warning, because this is the
             * last page before the user starts typing 24 words. */
            oled_draw_string(3, 0, "Not saved anywhere");
        }

        char line[22];
        snprintf(line, sizeof(line), "%s 12", entry_length_choice == 12 ? ">" : " ");
        oled_draw_string(4, 0, line);
        snprintf(line, sizeof(line), "%s 24", entry_length_choice == 24 ? ">" : " ");
        oled_draw_string(5, 0, line);

        oled_draw_string(7, 0, HINT_IMPORT_LENGTH);
        return;
    }

    char header[22];
    /* The mode is on the header row of every word, not just the first. Twelve
     * to twenty-four screens pass between the warning and the last word, and a
     * user who has forgotten which flow they are in is exactly the user who
     * reboots afterwards. */
    snprintf(header, sizeof(header), entry_is_temporary ? "TEMP word %d/%d"
                                                        : "Word %d/%d",
             entry.current_word + 1, entry.target_words);
    oled_draw_string_centered(0, header);

    char prefix_display[MNEMONIC_ENTRY_WORD_LEN + 24];
    entry_selector_line(prefix_display, sizeof(prefix_display));
    oled_draw_string(2, 0, "Type:");
    oled_draw_string(2, 36, prefix_display);

    if (entry_error[0] != '\0') {
        oled_draw_string(4, 0, entry_error);
    } else {
        char line[22];
        if (entry.word_mode) {
            /* The selector is already naming a whole word, so repeating the
             * first match would point at a different word than ACCEPT takes.
             * Show the position in the shortlist instead. */
            snprintf(line, sizeof(line), "%d of %d",
                     entry.option_index + 1, entry.option_count);
            oled_draw_string(4, 0, "Choice:");
            oled_draw_string(4, 48, line);
        } else {
            const char *suggestion = mnemonic_entry_suggestion(&entry);
            if (suggestion) {
                int n = mnemonic_entry_match_count(&entry, 100);
                if (n > 1) {
                    snprintf(line, sizeof(line), "%s +%d", suggestion, n - 1);
                } else {
                    snprintf(line, sizeof(line), "%s", suggestion);
                }
                oled_draw_string(4, 0, "Match:");
                oled_draw_string(4, 42, line);
            }
        }
    }

    /* With a block open, BACK closes it instead of deleting a character, so
     * the hint has to say which one the next press will do. */
    oled_draw_string(7, 0, entry.in_group ? HINT_WORD_GROUP : HINT_SCROLL_DEL_SEL);
}

/* Import the phrase now held in `entry`. Always clears it before returning. */
static void mnemonic_entry_finish(void)
{
    char full_mnemonic[300];
    mnemonic_entry_build(&entry, full_mnemonic, sizeof(full_mnemonic));

    if (!wallet_validate_mnemonic(full_mnemonic)) {
        /* Almost always a mistyped word - the checksum catches it here rather
         * than after the user has trusted a wrong wallet. */
        ESP_LOGW(TAG, "Invalid mnemonic checksum");
        snprintf(entry_error, sizeof(entry_error), "Bad checksum");
        memzero(full_mnemonic, sizeof(full_mnemonic));
        mnemonic_entry_clear(&entry);
        return;
    }

    if (!ensure_wallet_unlocked()) {
        memzero(full_mnemonic, sizeof(full_mnemonic));
        mnemonic_entry_clear(&entry);
        ui_set_screen(SCREEN_MAIN_MENU);
        return;
    }

    if (entry_is_temporary) {
        /* The one line that differs from an import: the phrase goes into RAM
         * and nothing else happens to it. No slot is allocated, no ciphertext
         * is written, no wallet count moves, and the backup bitmask - which
         * would record that a seed exists here - is not touched. */
        WalletError terr = wallet_use_temporary_mnemonic(full_mnemonic);
        memzero(full_mnemonic, sizeof(full_mnemonic));
        mnemonic_entry_clear(&entry);

        if (terr != WALLET_OK) {
            ESP_LOGE(TAG, "Temporary seed rejected: %d", terr);
            ui_set_screen(SCREEN_MAIN_MENU);
            return;
        }

        /* The longer default takes effect now rather than at the warning
         * screen, so an abandoned entry leaves the user's own timeout alone. */
        lock_timeout_choice = LOCK_TIMEOUT_TEMP_SEED;
        ESP_LOGI(TAG, "Temporary seed active; auto-lock %s",
                 lock_timeout_label(lock_timeout_choice));
        ui_set_screen(SCREEN_WALLET_INFO);
        return;
    }

    uint8_t idx = wallet_add_mnemonic(full_mnemonic);
    memzero(full_mnemonic, sizeof(full_mnemonic));
    mnemonic_entry_clear(&entry);

    if (idx > 0) {
        wallet_select_wallet(idx);
        ESP_LOGI(TAG, "Imported wallet %d", idx);
        ui_set_screen(SCREEN_WALLET_INFO);
    } else {
        ESP_LOGE(TAG, "Failed to import wallet");
        ui_set_screen(SCREEN_MAIN_MENU);
    }
}

static void screen_mnemonic_entry_on_button(button_id_t btn)
{
    entry_error[0] = '\0';

    if (entry_choosing_length) {
        switch (btn) {
            case BUTTON_UP:
            case BUTTON_DOWN:
                /* Two options, so either direction is a toggle. */
                entry_length_choice = (entry_length_choice == 12) ? 24 : 12;
                break;

            case BUTTON_CANCEL:
                mnemonic_entry_clear(&entry);
                ui_set_screen(SCREEN_MAIN_MENU);
                return;

            case BUTTON_ACCEPT:
                mnemonic_entry_reset(&entry, entry_length_choice);
                entry_choosing_length = false;
                ESP_LOGI(TAG, "Importing a %d-word phrase", entry_length_choice);
                break;

            default:
                break;
        }
        ui_invalidate();
        return;
    }

    switch (btn) {
        case BUTTON_UP:
            mnemonic_entry_scroll(&entry, -1);   /* see PICKER_FORWARD */
            break;

        case BUTTON_DOWN:
            mnemonic_entry_scroll(&entry, 1);
            break;

        case BUTTON_CANCEL:
            if (!mnemonic_entry_back(&entry)) {
                /* Backing out of the first character returns to the length
                 * question rather than leaving outright, so a wrong choice
                 * costs one press instead of a restart. */
                entry_choosing_length = true;
                mnemonic_entry_clear(&entry);
                mnemonic_entry_reset(&entry, entry_length_choice);
            }
            break;

        case BUTTON_ACCEPT:
            if (mnemonic_entry_accept(&entry) == MNEMONIC_ENTRY_ALL_DONE) {
                mnemonic_entry_finish();
            }
            break;

        default:
            break;
    }

    ui_invalidate();
}

/* ============================================================================
 * Settings Screen
 * ============================================================================ */

#ifdef CONFIG_ESP_WIFI_ENABLED
/* WiFi test functions - AP mode so other devices can see it */
static bool wifi_event_loop_created = false;

#define WIFI_AP_SSID     "LeekWallet"
#define WIFI_AP_PASS     "leek1234"
#define WIFI_AP_CHANNEL  1
#define WIFI_AP_MAX_CONN 4

static void wifi_test_toggle(void)
{
    if (!wifi_enabled) {
        ESP_LOGI(TAG, "Enabling WiFi AP...");

        /* Create default event loop if not already created */
        if (!wifi_event_loop_created) {
            esp_err_t err = esp_event_loop_create_default();
            if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
                wifi_event_loop_created = true;
            }
        }

        /* Initialize netif */
        esp_netif_init();
        esp_netif_create_default_wifi_ap();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&cfg);

        /* Configure AP mode */
        wifi_config_t wifi_config = {
            .ap = {
                .ssid = WIFI_AP_SSID,
                .ssid_len = strlen(WIFI_AP_SSID),
                .channel = WIFI_AP_CHANNEL,
                .password = WIFI_AP_PASS,
                .max_connection = WIFI_AP_MAX_CONN,
                .authmode = WIFI_AUTH_WPA2_PSK,
            },
        };

        esp_wifi_set_mode(WIFI_MODE_AP);
        esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
        esp_wifi_start();

        wifi_enabled = true;
        entropy_set_wifi_active(true);
        ESP_LOGI(TAG, "WiFi AP enabled: SSID='%s' PASS='%s'", WIFI_AP_SSID, WIFI_AP_PASS);
    } else {
        ESP_LOGI(TAG, "Disabling WiFi...");
        esp_wifi_stop();
        esp_wifi_deinit();
        wifi_enabled = false;
        entropy_set_wifi_active(false);
        ESP_LOGI(TAG, "WiFi disabled");
    }
}
#endif /* CONFIG_ESP_WIFI_ENABLED */

/* BLE lives in ble.c and is reached only through transport.c: the settings
 * screen selects a link, it does not drive a radio. The ad-hoc NimBLE toggle
 * that used to sit here advertised forever and served nothing, and it could be
 * on at the same time as the USB endpoint. */

/* USB HID test function */
static void usb_hid_test(void)
{
#ifdef CONFIG_TINYUSB_ENABLED
    /* It typed an address; there is no Zcash one yet. */
    ESP_LOGW(TAG, "USB HID test - nothing to type yet");
#else
    ESP_LOGW(TAG, "TinyUSB not enabled in sdkconfig");
    ESP_LOGI(TAG, "To enable: CONFIG_TINYUSB_ENABLED=y");
#endif
}

static void screen_settings_enter(void)
{
    ESP_LOGI(TAG, "Settings screen");
    settings_selection = 0;
}

static void screen_settings_render(void)
{
    oled_clear();
    oled_draw_string_centered(0, "-- Settings --");

    /* Scrollable menu (3 visible at a time) */
    int start = (settings_selection > 1) ? settings_selection - 1 : 0;
    if (start > SETTINGS_ITEMS - 3) {
        start = SETTINGS_ITEMS - 3;
    }
    if (start < 0) start = 0;

    for (int i = 0; i < 3 && (start + i) < SETTINGS_ITEMS; i++) {
        int item_idx = start + i;
        char line[22];

        /* Show status for toggles */
#ifdef CONFIG_ESP_WIFI_ENABLED
        if (item_idx == SET_WIFI) {
            if (item_idx == settings_selection) {
                snprintf(line, sizeof(line), "> WiFi %s", wifi_enabled ? "[ON]" : "[OFF]");
            } else {
                snprintf(line, sizeof(line), "  WiFi %s", wifi_enabled ? "[ON]" : "[OFF]");
            }
        } else
#endif
        if (item_idx == SET_TRANSPORT) {
            /* Named for what it is rather than "BLE [ON]": the two are
             * exclusive, so a per-radio on/off would imply a state the device
             * cannot be in (PROTOCOL.md 3b). */
            snprintf(line, sizeof(line), "%s Link %s",
                     item_idx == settings_selection ? ">" : " ",
                     transport_label(transport_get()));
        } else if (item_idx == SET_BLE_NAME) {
            /* The name is on the menu line because it is broadcast to
             * everyone in range, so it should be visible without going
             * looking for it. Truncated for
             * the 21-column display only - what goes on air is whatever
             * ble_name_get() returns. */
            snprintf(line, sizeof(line), "%s %.14s",
                     item_idx == settings_selection ? ">" : " ",
                     ble_name_get());
        } else if (item_idx == SET_ACCOUNT) {
            /* The value is on the menu line, like Link and Blind: which
             * identity the device is deriving from is not something a user
             * should have to open a screen to discover. */
            snprintf(line, sizeof(line), "%s Account %u",
                     item_idx == settings_selection ? ">" : " ",
                     (unsigned)hd_account);
        } else if (item_idx == SET_NETWORK) {
            /* On the line for the same reason as Account: it decides which
             * addresses Receive shows. */
            snprintf(line, sizeof(line), "%s Net %s",
                     item_idx == settings_selection ? ">" : " ",
                     zcash_network ? "Testnet" : "Mainnet");
        } else if (item_idx == SET_BRIGHTNESS) {
            snprintf(line, sizeof(line), "%s Bright %s",
                     item_idx == settings_selection ? ">" : " ",
                     brightness_label(brightness_choice));
        } else if (item_idx == SET_ENTRY_STYLE) {
            snprintf(line, sizeof(line), "%s Entry %s",
                     item_idx == settings_selection ? ">" : " ",
                     mnemonic_entry_blocks_enabled() ? "Blocks" : "Simple");
#if UI_HAS_KNOB_SETTING
        } else if (item_idx == SET_KNOB) {
            /* "> Knob Reversed" is 15 columns of the 21. */
            snprintf(line, sizeof(line), "%s Knob %s",
                     item_idx == settings_selection ? ">" : " ",
                     button_knob_reversed() ? "Reversed" : "Normal");
#endif
        } else if (item_idx == SET_AUTOLOCK) {
            snprintf(line, sizeof(line), "%s Lock %s",
                     item_idx == settings_selection ? ">" : " ",
                     lock_timeout_label(lock_timeout_choice));
        } else {
            snprintf(line, sizeof(line), "%s %s",
                     item_idx == settings_selection ? ">" : " ",
                     settings_items[item_idx]);
        }
        oled_draw_string(2 + i * 2, 0, line);
    }

    oled_draw_string(7, 0, HINT_SETTINGS);
}

static void screen_settings_on_button(button_id_t btn)
{
    switch (btn) {
        case BUTTON_UP:
            if (settings_selection > 0) {
                settings_selection--;
            }
            break;

        case BUTTON_DOWN:
            if (settings_selection < SETTINGS_ITEMS - 1) {
                settings_selection++;
            }
            break;

        case BUTTON_CANCEL:
            ui_set_screen(SCREEN_MAIN_MENU);
            break;

        case BUTTON_ACCEPT:
            switch ((SettingsAction)settings_selection) {
                case SET_SHOW_SEED:
                    /* Re-entering the PIN is the point: this reveals the seed,
                     * so it must not ride on a session unlocked minutes ago.
                     *
                     * A full lock, not just the PIN gate, and that is a
                     * decision rather than a tidy-up. The question this screen
                     * asks is "prove you are the owner again", and holding a
                     * decrypted mnemonic and a live seed cache in RAM while
                     * asking it answers a different question - it is the state
                     * AUDIT S5 was written about, entered on purpose. The cost
                     * is one re-derivation the correct PIN pays for anyway;
                     * the benefit is that a device sitting on a PIN prompt has
                     * no seed in memory no matter how it got there. */
                    if (wallet_has_temporary_mnemonic()) {
                        /* Locking is what this menu item does, and locking is
                         * what destroys a temporary seed. Someone who has just
                         * typed 24 words and wants to check them would lose
                         * them to the act of asking -- the one outcome nobody
                         * would choose on purpose.
                         *
                         * There is also nothing to show: the phrase came from
                         * the user's own paper a moment ago, and no stored
                         * seed is reachable while a temporary one is loaded.
                         * So this goes to the screen that explains the mode
                         * rather than silently doing nothing or destroying
                         * what it was asked to display. */
                        ui_set_screen(SCREEN_TEMP_SEED);
                        break;
                    }
                    /* Said first, with a way back: past this point the only
                     * way into the menu again is the PIN. */
                    ui_set_screen(SCREEN_SHOW_SEED_CONFIRM);
                    break;
                case SET_NEW_WALLET:
                    /* The same route as the main menu's New Wallet, and for
                     * the same reason: the entropy gate is 128 bits for 12
                     * words and 256 for 24, so the length has to be chosen
                     * before the screen that measures against it. Two entry
                     * points to one flow is exactly how one of them keeps the
                     * old behaviour -- this one did. */
                    create_entropy_ready = false;
                    ui_set_screen(SCREEN_WALLET_CREATE);
                    break;
                case SET_IMPORT_WALLET:
                    ui_set_screen(SCREEN_MNEMONIC_ENTRY);
                    break;
                case SET_PASSPHRASE:
                    ui_set_screen(SCREEN_PASSPHRASE);
                    break;
                case SET_ACCOUNT:
                    /* Cycles, like Brightness and Auto-lock. Ten presses to
                     * cross the range is acceptable for something changed
                     * rarely, and it costs no extra screen; the wallet screen
                     * shows the resulting account, which is where the change
                     * is actually confirmed. */
                    hd_account_set(hd_account + 1);
                    ESP_LOGI(TAG, "Account %u selected", (unsigned)hd_account);
                    break;
                case SET_NETWORK:
                    zcash_network_toggle();
                    ESP_LOGI(TAG, "Network: %s", zcash_network ? "testnet" : "mainnet");
                    break;
                case SET_ENTRY_STYLE:
                    entry_blocks_apply(!mnemonic_entry_blocks_enabled());
                    entry_blocks_save();
                    ESP_LOGI(TAG, "Word entry: %s",
                             mnemonic_entry_blocks_enabled() ? "blocks" : "simple");
                    break;

#if UI_HAS_KNOB_SETTING
                case SET_KNOB:
                    knob_reverse_toggle();
                    ESP_LOGI(TAG, "Knob direction: %s",
                             button_knob_reversed() ? "reversed" : "normal");
                    break;
#endif

                case SET_BRIGHTNESS:
                    brightness_choice = brightness_next(brightness_choice);
                    brightness_apply_and_save();
                    ESP_LOGI(TAG, "Brightness set to %s",
                             brightness_label(brightness_choice));
                    break;
                case SET_AUTOLOCK:
                    lock_timeout_choice = (lock_timeout_choice + 1) % (int)LOCK_TIMEOUT_COUNT;
                    lock_timeout_save();
                    ESP_LOGI(TAG, "Auto-lock set to %s",
                             lock_timeout_label(lock_timeout_choice));
                    break;
                case SET_CHANGE_PIN:
                    ui_set_screen(SCREEN_PIN_CHANGE);
                    break;
#ifdef CONFIG_ESP_WIFI_ENABLED
                case SET_WIFI: wifi_test_toggle(); break;
#endif
                case SET_TRANSPORT:
                    /* Turning one on turns the other off and kills any
                     * session; transport.c is the only place that may. */
                    /* transport_apply() reports the radio to the entropy gate
                     * itself, at boot as well as here. */
                    transport_toggle();
                    break;
                case SET_BLE_NAME:
                    ui_set_screen(SCREEN_BLE_NAME);
                    break;
                case SET_USB:  usb_hid_test();     break;
                case SET_WIPE:
                    ui_set_screen(SCREEN_WIPE_CONFIRM);
                    break;
                case SET_BACK:
                    ui_set_screen(SCREEN_MAIN_MENU);
                    break;
                default:
                    break;
            }
            break;

        default:
            break;
    }

    ui_invalidate();
}

/* ============================================================================
 * QR Out Screen
 *
 * The return path (qr-out.h). qr-out.c owns which
 * part is up and guarantees it fits; this only draws it, steps it on a timer
 * from service_qr_out(), and lets the user try the three module sizes, since
 * which one a camera reads off this panel is a bench question.
 *
 * No stack: `qr_out_back` is where BACK goes, set by whoever called
 * ui_show_ur().
 * ============================================================================ */

static screen_id_t qr_out_back = SCREEN_MAIN_MENU;

bool ui_show_ur(const char *type, const uint8_t *cbor, size_t len,
                screen_id_t back)
{
    const int64_t now = esp_timer_get_time();
    /* Most readable first. v3 x2 cannot carry everything, so a message that
       does not fit there falls back to the next most readable mode, and one
       that fits nowhere is refused rather than half shown. */
    static const uint8_t ORDER[] = { QR_OUT_MODE_DEFAULT, 0, 1 };
    bool started = false;
    for (size_t i = 0; i < sizeof ORDER && !started; i++) {
        started = qr_out_start(type, cbor, len, ORDER[i], now);
    }
    if (!started) {
        return false;
    }
    qr_out_back = back;
    ui_set_screen(SCREEN_QR_OUT);
    return true;
}

static void screen_qr_out_render(void)
{
    const char *frame = qr_out_frame();
    const QrOutMode *m = qr_out_mode_info(qr_out_current_mode());
    if (frame == NULL || m == NULL ||
        oled_draw_qrcode_ex(frame, m->version, m->scale, m->inverted) != ESP_OK) {
        /* qr-out.c measured every frame before showing one, so this is a
           bug rather than a state; say so instead of a blank panel. */
        oled_clear();
        oled_draw_string_centered(3, "QR unavailable");
        oled_draw_string(7, 0, HINT_BACK);
    }
}

static void screen_qr_out_on_button(button_id_t btn)
{
    const int64_t now = esp_timer_get_time();
    switch (btn) {
        case BUTTON_UP:
        case BUTTON_DOWN: {
            /* Step through the module sizes. A mode that cannot carry this
               message is skipped rather than shown blank. */
            const uint8_t step = (btn == BUTTON_UP) ? QR_OUT_MODE_COUNT - 1 : 1;
            uint8_t mode = qr_out_current_mode();
            for (int i = 0; i < QR_OUT_MODE_COUNT - 1; i++) {
                mode = (uint8_t)((mode + step) % QR_OUT_MODE_COUNT);
                if (qr_out_set_mode(mode, now)) {
                    break;
                }
            }
            break;
        }
        case BUTTON_ACCEPT:
            /* Brightness for this QR only; see screen_qr_out_enter. */
            qr_brightness = brightness_next(qr_brightness);
            oled_set_contrast(BRIGHTNESS_LEVELS[qr_brightness]);
            break;
        case BUTTON_CANCEL:
            ui_set_screen(qr_out_back);
            break;
        default:
            break;
    }
    ui_invalidate();
}

/*
 * The QR has its own brightness: Dim on entry, stepped with OK.
 *
 * An earlier version forced full brightness here, on the theory that contrast
 * limits a blurred read. It did not help the first webcam, and it can hurt: an
 * OLED at full drive blooms on a camera sensor and the dark modules drown.
 * Starting from the user's own level was no better: at High a phone camera
 * showed rolling lines across the panel - its shutter beating against the
 * OLED's refresh - where at Dim it read cleanly. So Dim for every QR,
 * pairing and signature alike, whatever the panel is set to elsewhere; OK
 * still steps it for a camera that wants otherwise.
 */
static void screen_qr_out_enter(void)
{
    qr_brightness = 5;   /* Dim */
    oled_set_contrast(BRIGHTNESS_LEVELS[qr_brightness]);
    oled_set_fast_refresh(true);   /* no dark bands on a camera; oled.h */
}

static void screen_qr_out_exit(screen_id_t next)
{
    (void)next;
    qr_out_stop();
    oled_set_fast_refresh(false);
    oled_set_contrast(BRIGHTNESS_LEVELS[brightness_choice]);
}

/* Advance the animation. Called once per turn of the UI task's loop, which
 * wakes at least every 100 ms, so a frame period is honoured to within that. */
static void service_qr_out(void)
{
    if (ui_get_screen() != SCREEN_QR_OUT) {
        return;
    }
    if (qr_out_tick(esp_timer_get_time())) {
        ui_invalidate();
    }
}

/* The footer of a passphrase keyboard (SD card, device passphrase). CANCEL
 * closes an open block, otherwise deletes (text_entry_back()); ACCEPT opens a
 * block, otherwise picks. The four-button wording only ever said BCK, so its
 * GROUP and SEL hints are the same string. */
static const char *text_entry_hint(const TextEntry *e)
{
    if (e->in_group) {
        return HINT_TEXT_GROUP;
    }
    return text_entry_on_group(e) ? HINT_TEXT_OPEN : HINT_TEXT_SEL;
}

/* ============================================================================
 * SD Card Screen (roadmap 1.4)
 *
 * A list of the card's .zkv files, plus "Save open seed" when there is one.
 * Picking a file asks for its passphrase and loads it as the session's seed:
 * nothing reaches flash, and a lock or a reboot forgets it, exactly like a
 * typed temporary seed (the home screen then reads TEMP and its ZFP).
 * Saving asks for a new passphrase twice and writes SEEDn.ZKV, never over an
 * existing file, and reads it back before saying so. sd-seed.c does the work
 * and the host suite checks it; this is only the four-button front of it.
 *
 * The KDF takes a few seconds on the S3, so the panel says so first.
 * ============================================================================ */

#if UI_HAS_SD_SEED
typedef enum { SD_LIST, SD_PASS_LOAD, SD_PASS_NEW, SD_PASS_REPEAT, SD_RESULT } SdStage;

static SdStage   sd_stage;
static char      sd_names[SDCARD_FILES_MAX][SDCARD_NAME_MAX + 1];
static int       sd_count;          /* -1: no card */
static int       sd_sel;
static bool      sd_can_save;
static bool      sd_can_sign;       /* LEEKZ/unsigned.pczt is on the card */
static bool      sd_go_home;        /* RESULT: OK goes to the home screen */
static TextEntry sd_entry;
static char      sd_first[TEXT_ENTRY_MAX + 1];
static char      sd_msg[4][22];
static int       sd_load_index = -1;  /* which seed file the passphrase is for */

/* Items, in order: "Sign transaction" when there is one, the seed files,
 * then "Save open seed". */
static int sd_items(void)
{
    if (sd_count < 0) return 0;
    return (sd_can_sign ? 1 : 0) + sd_count + (sd_can_save ? 1 : 0);
}

/* The seed file at list position `idx`, or -1 for the two actions. */
static int sd_file_at(int idx)
{
    if (sd_can_sign) idx--;
    return (idx >= 0 && idx < sd_count) ? idx : -1;
}

static void sd_rescan(void)
{
    /* Unmount first: the card may have been swapped since the last look. */
    sdcard_unmount();
    sd_count = sdseed_list(sd_names, SDCARD_FILES_MAX);
    WalletStatus st = wallet_get_status();
    sd_can_save = st.wallet_count > 0 || wallet_has_temporary_mnemonic();
    /* Signing needs a seed as much as saving does. */
    sd_can_sign = sd_count >= 0 && sd_can_save && sdsign_available();
    sd_sel = 0;
}

static void sd_result(bool home, const char *a, const char *b, const char *c, const char *d)
{
    const char *lines[4] = { a, b, c, d };
    for (int i = 0; i < 4; i++) snprintf(sd_msg[i], sizeof sd_msg[i], "%s", lines[i] ? lines[i] : "");
    sd_go_home = home;
    sd_stage = SD_RESULT;
}

static void sd_busy(const char *what)
{
    oled_clear();
    oled_draw_string_centered(3, what);
    oled_draw_string_centered(4, "a few seconds");
    oled_refresh();
}

static void sd_forget(void)
{
    text_entry_clear(&sd_entry);
    memzero(sd_first, sizeof sd_first);
}

static void sd_load(void)
{
    const char *name = sd_names[sd_load_index >= 0 ? sd_load_index : 0];
    if (!ensure_wallet_unlocked()) {
        sd_forget();
        sd_result(false, "Not loaded", "Unlock failed", NULL, NULL);
        return;
    }
    sd_busy("Opening...");
    ZkvContents meta;
    SdSeedResult r = sdseed_load(name, sd_entry.text, (size_t)sd_entry.length, &meta);
    sd_forget();
    if (r != SDSEED_OK) {
        sd_result(false, "Not loaded", sdseed_strerror(r), NULL, NULL);
        return;
    }
    /* The file says which account it is for; Receive follows it, and the
     * birthday is there for a companion's zGetAccount (session only: this
     * is a temporary seed). */
    if (meta.account < HD_ACCOUNT_COUNT) {
        hd_account_set(meta.account);
    }
    ZcashMeta zm;
    memset(&zm, 0, sizeof zm);
    zm.birthday = meta.birthday;
    zm.flags = meta.flags & ZMETA_FLAG_USE_QSK;
    snprintf(zm.label, sizeof zm.label, "%s", meta.label);
    zmeta_set(meta.account, zcash_network, &zm);
    /* Wider than a row; sd_result() cuts each line to the panel. */
    char l1[40], l2[40], l3[40];
    snprintf(l1, sizeof l1, "Loaded %s", name);
    snprintf(l2, sizeof l2, "%s", meta.label[0] ? meta.label : "(no label)");
    snprintf(l3, sizeof l3, "acct %u from %u", (unsigned)meta.account, (unsigned)meta.birthday);
    sd_result(true, l1, l2, l3, "Gone on lock/reboot");
    memzero(&meta, sizeof meta);
}

static void sd_save(void)
{
    if (strcmp(sd_first, sd_entry.text) != 0) {
        sd_forget();
        sd_result(false, "Not saved", "Passphrases differ", NULL, NULL);
        return;
    }
    if (!ensure_wallet_unlocked()) {
        sd_forget();
        sd_result(false, "Not saved", "Unlock failed", NULL, NULL);
        return;
    }
    sd_busy("Sealing...");
    char name[SDCARD_NAME_MAX + 1];
    /* What the device knows about the account goes on the card with it. */
    ZcashMeta zm;
    zmeta_get(hd_account, zcash_network, &zm);
    SdSeedResult r = sdseed_save(sd_entry.text, (size_t)sd_entry.length, zm.label, zm.birthday,
                                 hd_account, 0, name);
    sd_forget();
    if (r != SDSEED_OK) {
        sd_result(false, "Not saved", sdseed_strerror(r), NULL, NULL);
        return;
    }
    char l1[40];
    snprintf(l1, sizeof l1, "Saved %s", name);
    sd_result(false, l1, "Keep the card and", "the passphrase:", "both are needed");
}
/* Airgap signing (roadmap 5): read LEEKZ/unsigned.pczt, check it as a USB
 * upload is checked, and hand it to the same review screen. */
static void sd_sign_start(void)
{
    if (!ensure_wallet_unlocked()) {
        sd_result(false, "Not signed", "Unlock failed", NULL, NULL);
        ui_invalidate();
        return;
    }
    sd_busy("Checking...");
    const zs_summary *summary = NULL;
    int rc = sdsign_load(hd_account, zcash_network, &summary);
    button_drain();
    if (rc != 0 || !summary) {
        sd_result(false, "Not signed", sdsign_strerror(rc), NULL, NULL);
        ui_invalidate();
        return;
    }
    zsign_summary = summary;
    zsign_network = zcash_network;
    zsign_from_sd = true;
    ui_set_screen(SCREEN_ZSIGN_REVIEW);
}

/* The review's answer for a card transaction. */
static void sd_sign_review_done(bool approved)
{
    zsign_from_sd = false;
    if (!approved) {
        sdsign_cancel();
        sd_result(false, "Not signed", "Rejected", NULL, NULL);
        ui_set_screen(SCREEN_SD_SEED);
        sd_stage = SD_RESULT;
        return;
    }
    sd_busy("Signing...");
    uint32_t n = 0;
    int rc = sdsign_finish(&n);
    button_drain();
    if (rc != 0) {
        sd_result(false, "Not signed", sdsign_strerror(rc), NULL, NULL);
    } else {
        char l1[40];
        snprintf(l1, sizeof l1, "Signed: %u signature%s", (unsigned)n, n == 1 ? "" : "s");
        sd_result(false, l1, "Wrote LEEKZ/", "signed.zsig", "Take the card to the PC");
    }
    ui_set_screen(SCREEN_SD_SEED);
    sd_stage = SD_RESULT;
}
#endif /* UI_HAS_SD_SEED */

static void screen_sd_seed_enter(void)
{
#if UI_HAS_SD_SEED
    sd_stage = SD_LIST;
    sd_forget();
    sd_rescan();
#endif
}

static void screen_sd_seed_exit(screen_id_t next)
{
    (void)next;
#if UI_HAS_SD_SEED
    sd_forget();
#endif
}

static void screen_sd_seed_render(void)
{
    oled_clear();
#if UI_HAS_SD_SEED
    switch (sd_stage) {
        case SD_LIST: {
            oled_draw_string_centered(0, "SD Card");
            if (sd_count < 0) {
                oled_draw_string_centered(3, "No SD card");
                oled_draw_string(7, 0, HINT_SD_RETRY);
                return;
            }
            const int n = sd_items();
            if (n == 0) {
                oled_draw_string_centered(3, "No seed files");
                oled_draw_string(7, 0, HINT_SD_RETRY);
                return;
            }
            int start = sd_sel > 2 ? sd_sel - 2 : 0;
            for (int i = 0; i < 4 && start + i < n; i++) {
                const int idx = start + i;
                char line[22];
                const int file = sd_file_at(idx);
                snprintf(line, sizeof line, "%s %s", idx == sd_sel ? ">" : " ",
                         file >= 0 ? sd_names[file]
                         : (sd_can_sign && idx == 0) ? "Sign transaction" : "Save open seed");
                oled_draw_string(2 + i, 0, line);
            }
            oled_draw_string(7, 0, HINT_SD_LIST);
            return;
        }
        case SD_PASS_LOAD:
        case SD_PASS_NEW:
        case SD_PASS_REPEAT: {
            oled_draw_string_centered(0, sd_stage == SD_PASS_LOAD ? "Card passphrase"
                                         : sd_stage == SD_PASS_NEW ? "New card passphrase"
                                                                   : "Repeat passphrase");
            const int len = sd_entry.length;
            const char *tail = len > 18 ? sd_entry.text + (len - 18) : sd_entry.text;
            char shown[24];
            size_t out = 0;
            if (len > 18) shown[out++] = '<';
            for (size_t i = 0; tail[i] != '\0' && out < sizeof(shown) - 1; i++) shown[out++] = tail[i];
            shown[out] = '\0';
            oled_draw_string(2, 0, len ? shown : "(8-64 characters)");
            char prev[8], cur[8], next[8], sel[32];
            text_entry_label_offset(&sd_entry, -1, prev, sizeof prev);
            text_entry_label_offset(&sd_entry, 0, cur, sizeof cur);
            text_entry_label_offset(&sd_entry, 1, next, sizeof next);
            snprintf(sel, sizeof sel, "%s <%s> %s", prev, cur, next);
            oled_draw_string_centered(4, sel);
            char count[22];
            snprintf(count, sizeof count, "%u chars", (unsigned)len & 0x7F);
            oled_draw_string_centered(5, count);
            oled_draw_string(7, 0, text_entry_hint(&sd_entry));
            return;
        }
        case SD_RESULT:
            for (int i = 0; i < 4; i++) oled_draw_string_centered(1 + i + (i > 0), sd_msg[i]);
            oled_draw_string(7, 0, HINT_OK);
            return;
    }
#else
    oled_draw_string_centered(0, "SD Card");
    oled_draw_string_centered(3, "No card slot");
    oled_draw_string(7, 0, HINT_BACK);
#endif
}

static void screen_sd_seed_on_button(button_id_t btn)
{
#if UI_HAS_SD_SEED
    switch (sd_stage) {
        case SD_LIST: {
            const int n = sd_items();
            if (btn == BUTTON_CANCEL) {
                ui_set_screen(SCREEN_MAIN_MENU);
                return;
            }
            if (btn == BUTTON_DOWN && sd_sel < n - 1) sd_sel++;
            if (btn == BUTTON_UP && sd_sel > 0) sd_sel--;
            if (btn == BUTTON_ACCEPT) {
                if (sd_count < 0 || n == 0) {
                    sd_rescan();
                } else if (sd_can_sign && sd_sel == 0) {
                    sd_sign_start();
                    return;
                } else {
                    text_entry_reset(&sd_entry);
                    const int file = sd_file_at(sd_sel);
                    sd_load_index = file;
                    sd_stage = file >= 0 ? SD_PASS_LOAD : SD_PASS_NEW;
                }
            }
            break;
        }
        case SD_PASS_LOAD:
        case SD_PASS_NEW:
        case SD_PASS_REPEAT:
            if (btn == BUTTON_UP) {
                text_entry_scroll(&sd_entry, -1);   /* PICKER_FORWARD */
            } else if (btn == BUTTON_DOWN) {
                text_entry_scroll(&sd_entry, 1);
            } else if (btn == BUTTON_CANCEL) {
                if (!text_entry_back(&sd_entry)) {
                    sd_forget();
                    sd_stage = SD_LIST;
                }
            } else if (btn == BUTTON_ACCEPT) {
                TextEntryResult r = text_entry_accept(&sd_entry);
                if (r == TEXT_ENTRY_CANCELLED) {
                    sd_forget();
                    sd_stage = SD_LIST;
                } else if (r == TEXT_ENTRY_DONE) {
                    if (!zkv_pass_ok(sd_entry.text, (size_t)sd_entry.length)) {
                        /* Said before a KDF runs, and the same on both paths:
                         * no .zkv passphrase can be shorter. */
                        sd_forget();
                        sd_result(false, "Passphrase must be", "8-64 characters", NULL, NULL);
                    } else if (sd_stage == SD_PASS_LOAD) {
                        sd_load();
                    } else if (sd_stage == SD_PASS_NEW) {
                        snprintf(sd_first, sizeof sd_first, "%s", sd_entry.text);
                        text_entry_clear(&sd_entry);
                        text_entry_reset(&sd_entry);
                        sd_stage = SD_PASS_REPEAT;
                    } else {
                        sd_save();
                    }
                }
            }
            break;
        case SD_RESULT:
            if (btn == BUTTON_ACCEPT || btn == BUTTON_CANCEL) {
                if (sd_go_home) {
                    ui_set_screen(SCREEN_MAIN_MENU);
                    return;
                }
                sd_stage = SD_LIST;
                sd_rescan();
            }
            break;
    }
#else
    if (btn == BUTTON_CANCEL) {
        ui_set_screen(SCREEN_MAIN_MENU);
        return;
    }
#endif
    ui_invalidate();
}

/* ============================================================================
 * Receive Screen (roadmap 1.6)
 *
 * The selected account's addresses on the network chosen under Settings:
 * the unified address whole, over as many pages as it takes, then as a QR;
 * the transparent address, marked public, then as a QR. UP/DN step through
 * the pages, OK flips between an address and its QR, BACK leaves. The pages
 * themselves are zcash-receive.c's, checked on the host page by page and
 * scanned back from the framebuffer.
 *
 * Derived once on entry: Sinsemilla on the Zcash worker, a second or so.
 * Nothing here is secret - viewing keys and addresses only - but it is wiped
 * on the way out like everything else that came from the seed.
 * ============================================================================ */

#if LEEK_HAS_ZCASH
static ZcashAccount recv_account;
static bool         recv_ready;
static int          recv_page;

/* A QR page gets what the QR return path gets (screen_qr_out_enter): Dim and
 * the fast refresh, which is what a phone camera reads this panel best at.
 * Text pages get the user's own level back. */
static void recv_apply_panel(void)
{
    ZRecvPage p;
    const bool qr = recv_ready && zrecv_render(&recv_account, recv_page, &p) &&
                    p.kind == ZRECV_QR;
    oled_set_contrast(BRIGHTNESS_LEVELS[qr ? 5 : brightness_choice]);
    oled_set_fast_refresh(qr);
}
#endif

static void screen_show_address_enter(void)
{
#if LEEK_HAS_ZCASH
    recv_ready = false;
    recv_page = 0;
    if (ensure_wallet_unlocked() &&
        zcash_account_derive(hd_account, zcash_network, &recv_account) == WALLET_OK) {
        recv_ready = true;
    }
#endif
}

static void screen_show_address_exit(screen_id_t next)
{
    (void)next;
#if LEEK_HAS_ZCASH
    memzero(&recv_account, sizeof recv_account);
    recv_ready = false;
    recv_page = 0;
    recv_apply_panel();
#endif
}

static void screen_show_address_render(void)
{
    oled_clear();
#if LEEK_HAS_ZCASH
    ZRecvPage p;
    if (!recv_ready || !zrecv_render(&recv_account, recv_page, &p)) {
        oled_draw_string_centered(0, "Receive");
        oled_draw_string_centered(3, "No address");
        oled_draw_string_centered(4, "(derivation failed)");
        oled_draw_string(7, 0, HINT_BACK);
        return;
    }
    if (p.kind == ZRECV_QR) {
        if (oled_draw_qrcode_at(p.qr, p.version, p.scale) != ESP_OK) {
            /* zcash-receive.c sized it for the panel; not a state. */
            oled_clear();
            oled_draw_string_centered(3, "QR unavailable");
            oled_draw_string(7, 0, HINT_BACK);
        }
        return;
    }
    for (int r = 0; r < ZRECV_ROWS; r++) {
        if (p.row[r][0]) {
            oled_draw_string(r, 0, p.row[r]);
        }
    }
#else
    oled_draw_string_centered(0, "Receive");
    oled_draw_string_centered(3, "No Zcash on this");
    oled_draw_string_centered(4, "board");
    oled_draw_string(7, 0, HINT_BACK);
#endif
}

static void screen_show_address_on_button(button_id_t btn)
{
    if (btn == BUTTON_CANCEL) {
        ui_set_screen(SCREEN_MAIN_MENU);
        return;
    }
#if LEEK_HAS_ZCASH
    if (recv_ready) {
        const int n = zrecv_page_count(&recv_account);
        switch (btn) {
            case BUTTON_DOWN:
                if (recv_page < n - 1) recv_page++;
                break;
            case BUTTON_UP:
                if (recv_page > 0) recv_page--;
                break;
            case BUTTON_ACCEPT:
                recv_page = zrecv_toggle_qr(&recv_account, recv_page);
                break;
            default:
                break;
        }
        recv_apply_panel();
    }
#endif
    ui_invalidate();
}

#ifdef LEEK_HOST_TEST
TextEntry *ui__sd_entry_for_test(void)
{
#if UI_HAS_SD_SEED
    return &sd_entry;
#else
    return NULL;
#endif
}

int ui__receive_page_for_test(void)
{
#if LEEK_HAS_ZCASH
    return recv_ready ? recv_page : -1;
#else
    return -1;
#endif
}
#endif

/* ============================================================================
 * Scan Screen
 *
 * The camera loop's host. Frames arrive from camera.c and each decoded symbol
 * is fed to the UR assembler. No UR type is accepted yet (airgap.h): a
 * complete message is refused and the screen says so. Phase 5 hands a
 * zcash-pczt to a confirmation from here. Nothing on this screen reads a
 * scanned message's contents out loud.
 * ============================================================================ */

static char scan_status[22];

/*
 * The viewfinder.
 *
 * Aiming a camera you cannot see through is guesswork, and the failure it
 * produces - "nothing is happening" - looks identical whether the QR is out of
 * frame, out of focus or absent. A 128x56 one-bit preview answers that at a
 * glance, and the status line below it answers the other half: whether the
 * device is reading and how much is left.
 *
 * `scan_preview_live` stays false until the camera has actually produced a
 * frame, so a board whose sensor did not start keeps the plain text screen
 * rather than showing an empty rectangle.
 *
 * Nothing decoded is ever drawn here. The preview is raw sensor pixels and the
 * status line is one of a fixed set of strings this file owns, so a QR code
 * cannot put words of its choosing on the trusted display.
 */
static uint8_t scan_preview[VIEWFINDER_BYTES];
static bool    scan_preview_live;

/* One decoded QR string: the same function service_scan() calls per frame,
 * named so the host can drive it without a camera. */
static void scan_handle(const char *ur, size_t len)
{
    switch (airgap_scan_feed(ur, len)) {
        case AIRGAP_SCAN_ACCEPTED:
            snprintf(scan_status, sizeof scan_status, "%lu parts to go",
                     (unsigned long)airgap_scan_remaining());
            break;
        case AIRGAP_SCAN_REDUNDANT:
            break;
        case AIRGAP_SCAN_UNREADABLE:
            snprintf(scan_status, sizeof scan_status, "Not a UR part");
            break;
        case AIRGAP_SCAN_WRONG_TYPE:
            snprintf(scan_status, sizeof scan_status, "Not a Zcash request");
            break;
        case AIRGAP_SCAN_PCZT: {
#if LEEK_HAS_ZCASH
            /* The same check and the same review as a USB upload. */
            const zs_summary *summary = NULL;
            snprintf(scan_status, sizeof scan_status, "Checking...");
            ui_render();
            int rc = ensure_wallet_unlocked()
                         ? airgap_load_pczt(hd_account, zcash_network, &summary)
                         : ZSIGN_E_LOCKED;
            button_drain();
            if (rc != 0 || !summary) {
                snprintf(scan_status, sizeof scan_status, "%.21s", zcash_sign_strerror(rc));
                break;
            }
            zsign_summary = summary;
            zsign_network = zcash_network;
            zsign_from_sd = false;
            zsign_from_qr = true;
            ui_set_screen(SCREEN_ZSIGN_REVIEW);
            return;
#else
            snprintf(scan_status, sizeof scan_status, "No Zcash here");
            break;
#endif
        }
    }
    ui_invalidate();
}

#if LEEK_HAS_ZCASH
/* The review's answer for a scanned PCZT: on ACCEPT sign and show the
 * signatures as ur:leek-zsig for the companion to scan; BACK there returns
 * to the menu. */
static void qr_sign_review_done(bool approved)
{
#if LEEK_HAS_ZCASH
    zsign_from_qr = false;
    if (approved) {
        static uint8_t cbor[QR_OUT_MAX_MESSAGE];
        size_t len = 0;
        int rc = airgap_sign(cbor, sizeof cbor, &len);
        button_drain();
        if (rc == 0 && ui_show_ur(AIRGAP_UR_ZSIG, cbor, len, SCREEN_MAIN_MENU)) {
            memzero(cbor, sizeof cbor);
            return;
        }
        memzero(cbor, sizeof cbor);
        ESP_LOGE(TAG, "QR signing failed: %d", rc);
    } else {
        airgap_cancel();
    }
#else
    (void)approved;
#endif
    ui_set_screen(SCREEN_MAIN_MENU);
}
#endif

static void screen_scan_enter(void)
{
    airgap_scan_reset();
    scan_preview_live = false;
    memset(scan_preview, 0, sizeof scan_preview);
    /* A camera that refuses to start is a screen that says so, not a hang and
       not a reboot. The rest of this screen works either way: BACK still
       leaves, and the host-driven test path still feeds it strings. */
    snprintf(scan_status, sizeof scan_status,
             camera_start() ? "Point at the QR" : "No camera");
}

static void screen_scan_render(void)
{
    oled_clear();

    if (scan_preview_live) {
        /* Pages 0-6 are the preview, page 7 is the status line. Into the
           framebuffer, not straight to the panel: one flush then paints the
           image and the text together, which is what stopped the viewfinder
           flickering. */
        for (uint8_t page = 0; page < VIEWFINDER_H / 8; page++) {
            oled_blit_page(page, &scan_preview[(size_t)page * VIEWFINDER_W],
                           VIEWFINDER_W);
        }
        oled_draw_string_centered(7, scan_status);
        return;
    }

    oled_draw_string_centered(0, "Scan request");
    oled_draw_string_centered(3, scan_status);
    oled_draw_string(7, 0, HINT_BACK);
}

static void screen_scan_on_button(button_id_t btn)
{
    if (btn == BUTTON_CANCEL) {
        ui_set_screen(SCREEN_MAIN_MENU);
        return;
    }
    /* Bench controls for comparing combinations; orientation stays f2.
       UP brighter, DOWN darker, OK steps the capture size, and OK with UP
       held toggles live video to the PC viewer.

       A knob cannot be held on UP, so on the S3 CAM kit (knob + Cancel, no
       K1/K2) the same toggle is press-and-turn: a turn while the knob is
       held down. The press itself still steps the size first. */
    if ((btn == BUTTON_UP || btn == BUTTON_DOWN) && button_is_pressed(BUTTON_ACCEPT)) {
        camera_set_stream(!camera_streaming());
    } else if (btn == BUTTON_ACCEPT) {
        if (button_is_pressed(BUTTON_UP)) {
            camera_set_stream(!camera_streaming());
        } else if (!camera_next_frame_size()) {
            /* Said on the screen, not left as a view that stopped moving;
               OK again moves on to the next size. */
            snprintf(scan_status, sizeof scan_status, "%u failed; OK=next",
                     (unsigned)camera_frame_width());
        }
    } else if (btn == BUTTON_UP || btn == BUTTON_DOWN) {
        camera_set_exposure_bias((int8_t)(camera_exposure_bias() +
                                          (btn == BUTTON_UP ? 1 : -1)));
    }
    ui_invalidate();
}

static void screen_scan_exit(screen_id_t next)
{
    (void)next;
    camera_stop();
    airgap_scan_reset();
    scan_preview_live = false;
    memset(scan_preview, 0, sizeof scan_preview);
}

/* Drain whatever the camera decoded since the last turn of the loop. Bounded,
 * so a camera producing frames faster than the loop turns cannot starve the
 * buttons. */
static void service_scan(void)
{
    if (ui_get_screen() != SCREEN_SCAN) {
        return;
    }
    static char frame[1100];
    size_t len = 0;
    for (int i = 0; i < 4 && camera_next_qr(frame, sizeof frame, &len); i++) {
        scan_handle(frame, len);
    }

    /* The preview costs no frame: camera.c renders it from a frame it has
       already handed to the decoder, one in every CAMERA_PREVIEW_EVERY. What
       arrives here is whatever the last of those produced, or nothing. */
    const uint8_t *shot = camera_preview_take();
    if (shot != NULL) {
        memcpy(scan_preview, shot, sizeof scan_preview);
        scan_preview_live = true;
        /* Once parts are landing, the status line says how many are left
           rather than still inviting the user to aim. */
        /*
         * What the camera is making of the view, on the panel, because the
         * person aiming it cannot watch a serial log. `seen` is codes quirc
         * located; the gap between seen and read is the diagnosis: nothing
         * seen means too small, too dim or out of frame, while seen without
         * read means the modules are not being resolved. None of it comes
         * from a decoded payload, only from counters.
         */
        uint32_t cam_frames = 0, cam_read = 0, cam_seen = 0;
        camera_stats(&cam_frames, &cam_read, &cam_seen);

        const uint32_t left = airgap_scan_remaining();
        if (left == 0) {
            /* "f0 s12 r0": flip mode, codes seen, codes read. Short because
               the row is 21 characters and the numbers matter more than the
               words. */
            /* "640 e-5 f123 s3 r1": capture width, exposure, frames
               captured, codes seen, codes read. Frames still climbing on a
               blank view means the camera is live and the picture is dark;
               frames stopped means capture itself stopped. */
            char line[48];
            snprintf(line, sizeof line, "%u e%d f%u s%u r%u",
                     (unsigned)camera_frame_width(),
                     (int)camera_exposure_bias(),
                     (unsigned)(cam_frames > 999 ? 999 : cam_frames),
                     (unsigned)(cam_seen > 99 ? 99 : cam_seen),
                     (unsigned)(cam_read > 99 ? 99 : cam_read));
            /* 20 characters at most in practice; the row holds 21. */
            snprintf(scan_status, sizeof scan_status, "%.21s", line);
        }
        if (left > 0) {
            /* Clamped at three digits, which is both what the row fits and
               more parts than any message this device accepts can be cut
               into. The count is the decoder's own, never anything a scanned
               frame chose. */
            snprintf(scan_status, sizeof scan_status, "%u parts to go",
                     (unsigned)(left > 999 ? 999 : left));
        }
        ui_invalidate();
    }
}

/* ============================================================================
 * Entropy Collection Screen
 *
 * Mandatory extra randomness before generating a seed, in two flavours, both
 * mixed with the hardware RNG and neither substituted for it (entropy.h).
 *
 *   - Button presses. What is harvested is not which button but the timing
 *     between presses. Worth 2 bits each, and that number is an estimate: both
 *     audits found the bits actually come from ui_task's dequeue jitter rather
 *     than from the human, and that variance has never been measured on a
 *     board.
 *   - Physical dice. Worth log2(6) = 2.585 bits each, and that number is
 *     arithmetic. This is why the mode exists: the health checks in entropy.c
 *     provably cannot see a source that is statistically clean and shallow -
 *     0 detections in 200,000 trials, which is Coldcard's 2021 defect exactly -
 *     so user-supplied entropy is the only layer covering it, and a layer that
 *     important should rest on a number that can be defended rather than
 *     modelled.
 *
 * The two compose without overlapping: a roll's own button presses are hashed
 * into the pool and credited zero bits, so the gate's total is a strict lower
 * bound with the dice-press jitter left over as margin.
 * ============================================================================ */

/* The gate, in bits rather than in presses. 128 is the strength of a 12-word
 * seed and the floor this screen has always enforced; expressing it as bits is
 * what lets presses and rolls be spent interchangeably against it.
 *
 * Pure press mode still means exactly 64 presses (64 x 2 bits), which is where
 * the count landed when BITS_PER_EVENT was corrected from 4 to 2. Pure dice mode
 * means 50 rolls: 50 x 2585/1000 = 129 bits. A hundred rolls is 258 bits, i.e.
 * full 256-bit strength from the dice alone, which anyone who wants it can reach
 * by carrying on rolling rather than being demanded of everyone. */
/* The gate follows the seed the user asked for, which is why the word count is
 * now chosen before this screen rather than after it.
 *
 * Collecting 128 bits and then choosing 24 words used to be possible, and it
 * quietly meant a 256-bit seed with 128 bits of user entropy behind it. That is
 * only a real difference if the hardware RNG is shallow -- but that is the
 * exact case this pool exists for, so it is the case the number has to be right
 * in. Asking for the length first costs one screen and makes the gate mean what
 * a reader would assume it means. */
static int entropy_target_bits(void)
{
    return create_word_count == 24 ? 256 : 128;
}

/* Mirrors DICE_MILLIBITS_PER_ROLL in entropy.c. Used only to turn a bits
 * shortfall back into a roll count for the display; the credited figure always
 * comes from entropy_dice_bits() so there is one source of truth for the claim. */
#define ENTROPY_MILLIBITS_PER_ROLL 2585

typedef enum {
    ENTROPY_MODE_CHOOSE = 0,
    ENTROPY_MODE_DICE,
    ENTROPY_MODE_PRESS,
} entropy_mode_t;

static entropy_mode_t entropy_mode = ENTROPY_MODE_CHOOSE;
static int  entropy_menu_sel = 0;
/* The face the selector is showing, which is NOT yet in the pool. It survives a
 * commit so that rolling the same number twice is one press, and so the screen
 * always displays a value the user last looked at rather than snapping back to
 * a default they then have to re-read. */
static uint8_t dice_face = 1;
/* The last committed face, echoed back so a misentry is visible immediately.
 * There is no undo, and cannot be: the pool is a running SHA-256 and a commit is
 * not reversible. That is acceptable precisely because a wrong roll is not a
 * security problem - it is still an unpredictable value mixed into a hash - but
 * a user who cannot tell what they entered will not trust the ceremony, so the
 * cost of the missing undo is paid in confirmation instead. */
static uint8_t dice_last = 0;

static bool entropy_target_met(void)
{
    return entropy_total_bits_estimate() >= entropy_target_bits();
}

/* How many more presses / rolls would finish the job, given what the OTHER
 * source has already contributed. Computed rather than fixed because a user who
 * rolled 30 dice should not then be told they still owe 64 presses. */
static int entropy_press_target(void)
{
    int need = entropy_target_bits() - entropy_total_bits_estimate();
    if (need <= 0) return entropy_user_event_count();
    return entropy_user_event_count() + (need + 1) / 2;
}

static int entropy_dice_target(void)
{
    int need = entropy_target_bits() - entropy_total_bits_estimate();
    if (need <= 0) return entropy_dice_roll_count();
    int rolls = (need * 1000 + ENTROPY_MILLIBITS_PER_ROLL - 1) / ENTROPY_MILLIBITS_PER_ROLL;
    return entropy_dice_roll_count() + rolls;
}

static void screen_entropy_enter(void)
{
    ESP_LOGI(TAG, "Entropy collection screen");
    /* Nothing may be listening while a seed is made. This is the first screen
     * of wallet creation, so suspending here covers the whole of it: the
     * collection, the generation, the words on screen and their verification.
     * screen_main_menu_enter() puts the link back, and every way out of this
     * flow -- finishing, cancelling, failing -- goes through the main menu.
     *
     * The device cannot tell what it is plugged into, so this is the part it
     * can enforce. The part it cannot is the cable: see docs/VAULT.md on
     * generating a seed on power alone. */
    transport_suspend();
    entropy_reset_user_pool();
    entropy_mode     = ENTROPY_MODE_CHOOSE;
    entropy_menu_sel = 0;
    dice_face        = 1;
    dice_last        = 0;
}

/* --------------------------------------------------------------- chooser */

static void screen_entropy_render_choose(void)
{
    oled_clear();
    oled_draw_string_centered(0, "Add Randomness");

    /* 32 rather than the 21 columns the display has: the compiler has to be able
     * to prove no truncation, and the bit totals are int-width to it. */
    char line[32];
    snprintf(line, sizeof(line), "dice %d = %d bits",
             entropy_dice_roll_count(), entropy_dice_bits());
    oled_draw_string(1, 0, line);
    snprintf(line, sizeof(line), HINT_ENTROPY_EVENTS " %d = %d bits",
             entropy_user_event_count(), entropy_user_bits_estimate());
    oled_draw_string(2, 0, line);

    bool ready = entropy_target_met();

    oled_draw_string(4, 0, entropy_menu_sel == 0 ? "> Roll dice" : "  Roll dice");
    oled_draw_string(5, 0, entropy_menu_sel == 1 ? "> " HINT_ENTROPY_ITEM : "  " HINT_ENTROPY_ITEM);
    if (ready) {
        oled_draw_string(6, 0, entropy_menu_sel == 2 ? "> Create wallet"
                                                     : "  Create wallet");
    } else {
        snprintf(line, sizeof(line), "  need %d bits",
                 entropy_target_bits() - entropy_total_bits_estimate());
        oled_draw_string(6, 0, line);
    }

    oled_draw_string(7, 0, HINT_ENTROPY_CHOOSE);
}

/* ------------------------------------------------------------ dice entry */

static void screen_entropy_render_dice(void)
{
    oled_clear();
    oled_draw_string_centered(0, "Physical Dice");

    /* The warning is on screen every frame, not once behind an OK button,
     * because the failure it prevents is a user who is *more* confident for
     * having done the ceremony wrong. A phone's dice app is an unauditable PRNG
     * on a networked computer: if the phone is compromised the attacker chose
     * this seed, and the user believes it is the strongest one they have ever
     * made. Rolling nothing at all is strictly safer than that. */
    /* "dice", not the singular "die", which is the technically correct word
     * and the wrong one here. Most readers of this screen do not have English
     * as a first language and know "dice"; and on a wallet's warning line the
     * word "die" reads for half a second as the verb. Grammar loses to being
     * understood at a glance. */
    oled_draw_string(1, 0, "Use REAL dice -");
    oled_draw_string(2, 0, "phone apps can lie");

    char line[22];
    snprintf(line, sizeof(line), "roll %d/%d  ~%d bits",
             entropy_dice_roll_count(), entropy_dice_target(),
             entropy_dice_bits());
    oled_draw_string(3, 0, line);

    /* All six faces, with the armed one bracketed. Showing the whole range
     * rather than a bare number means the value is read as a position as well
     * as a digit, which is what makes a slip visible before ROLL commits it. */
    char sel[20];
    int n = 0;
    for (int f = 1; f <= 6; f++) {
        sel[n++] = (f == dice_face) ? '[' : ' ';
        sel[n++] = (char)('0' + f);
        sel[n++] = (f == dice_face) ? ']' : ' ';
    }
    sel[n] = '\0';
    oled_draw_string_centered(5, sel);

    if (dice_last) {
        snprintf(line, sizeof(line), "entered: %d", dice_last);
        oled_draw_string_centered(6, line);
    } else {
        oled_draw_string_centered(6, "roll, then ROLL");
    }

    oled_draw_string(7, 0, HINT_DICE);
}

/* ---------------------------------------------------------- press entry */

static void screen_entropy_render_press(void)
{
    oled_clear();
    oled_draw_string_centered(0, "Add Randomness");

    int events = entropy_user_event_count();
    int target = entropy_press_target();

    char line[22];
    snprintf(line, sizeof(line), "%d / %d  (+%d bits)",
             events, target, entropy_user_bits_estimate());
    oled_draw_string_centered(2, line);

    /* A 16-cell bar so progress is legible at a glance. */
    char bar[19];
    int filled = (events >= target) ? 16 : (events * 16) / target;
    bar[0] = '[';
    for (int i = 0; i < 16; i++) {
        bar[1 + i] = (i < filled) ? '#' : '-';
    }
    bar[17] = ']';
    bar[18] = '\0';
    oled_draw_string_centered(4, bar);

    /* One job per button, and the footer says which (T61).
     *
     * UP and DOWN are the only samples; ACCEPT only ever proceeds. It used to
     * be a sample too until the target was met, which made the same press mean
     * "collect" and then "create a wallet" - the one press on this screen that
     * must not be reached by reflex. Until it does something it is drawn as
     * unavailable rather than as a third way to stir the pool. */
    if (entropy_target_met()) {
        oled_draw_string_centered(5, "Ready");
        oled_draw_string(7, 0, HINT_MIX_READY);
    } else {
        char remaining[22];
        snprintf(remaining, sizeof(remaining), "%d more to go", target - events);
        oled_draw_string_centered(5, remaining);
        /* No NEXT yet - the target is required, not suggested. CANCEL still
         * backs out so nobody is stuck on this screen. */
        oled_draw_string(7, 0, HINT_MIX_WAIT);
    }
}

static void screen_entropy_render(void)
{
    switch (entropy_mode) {
        case ENTROPY_MODE_DICE:  screen_entropy_render_dice();   break;
        case ENTROPY_MODE_PRESS: screen_entropy_render_press();  break;
        default:                 screen_entropy_render_choose(); break;
    }
}

static void screen_entropy_proceed(void)
{
    /* Counts and totals. The roll values themselves are entropy and never reach
     * a log line - the same rule the seed words are under. */
    ESP_LOGI(TAG, "Collected %d taps + %d rolls (~%d bits) for the pool",
             entropy_user_event_count(), entropy_dice_roll_count(),
             entropy_total_bits_estimate());
    /* Set before the screen change, because ui_set_screen() runs the enter
     * hook synchronously and that hook is what reads it. */
    create_entropy_ready = true;
    ui_set_screen(SCREEN_WALLET_CREATE);
}

static void screen_entropy_on_button(button_id_t btn)
{
    /* CANCEL never lowers the bar. From a collection mode it steps back to the
     * chooser, keeping everything already contributed - a user switching from
     * dice to taps is not restarting. From the chooser it abandons wallet
     * creation outright, which is the only escape offered: "do not create a
     * wallet", never "create one with less entropy". */
    if (btn == BUTTON_CANCEL) {
        if (entropy_mode != ENTROPY_MODE_CHOOSE) {
            entropy_mode = ENTROPY_MODE_CHOOSE;
            ui_invalidate();
            return;
        }
        ESP_LOGI(TAG, "Entropy collection cancelled at %d taps, %d rolls",
                 entropy_user_event_count(), entropy_dice_roll_count());
        entropy_reset_user_pool();
        dice_face = 1;
        dice_last = 0;
        ui_set_screen(SCREEN_MAIN_MENU);
        return;
    }

    if (entropy_mode == ENTROPY_MODE_CHOOSE) {
        int items = entropy_target_met() ? 3 : 2;
        switch (btn) {
            case BUTTON_UP:
                entropy_menu_sel = (entropy_menu_sel + items - 1) % items;
                break;
            case BUTTON_DOWN:
                entropy_menu_sel = (entropy_menu_sel + 1) % items;
                break;
            case BUTTON_ACCEPT:
                if (entropy_menu_sel == 0) {
                    entropy_mode = ENTROPY_MODE_DICE;
                } else if (entropy_menu_sel == 1) {
                    entropy_mode = ENTROPY_MODE_PRESS;
                } else {
                    screen_entropy_proceed();
                    return;
                }
                break;
            default:
                break;
        }
        ui_invalidate();
        return;
    }

    if (entropy_mode == ENTROPY_MODE_DICE) {
        /* UP and DOWN move the selector; ACCEPT is the only thing that commits,
         * and it commits a roll and nothing else. Proceeding to generation is
         * not reachable from this mode at all - it lives behind BCK, on the
         * chooser - so the button being pressed a hundred times in a row can
         * never become the button that creates a wallet.
         *
         * The selector wraps, which is what keeps entry at roughly two presses
         * per roll: the mean circular distance between two faces of a d6 is 1.5,
         * plus the commit. It also keeps the previous face armed, so a repeated
         * number costs a single press. */
        switch (btn) {
            case BUTTON_UP:   /* PICKER_FORWARD: DOWN is the next face */
                dice_face = (uint8_t)((dice_face + 4) % 6 + 1);
                break;
            case BUTTON_DOWN:
                dice_face = (uint8_t)(dice_face % 6 + 1);
                break;
            case BUTTON_ACCEPT:
                entropy_add_dice_roll(dice_face, (uint64_t)esp_timer_get_time());
                dice_last = dice_face;
                break;
            default:
                break;
        }
        ui_invalidate();
        return;
    }

    /* Press mode. ACCEPT is the "proceed" button and nothing else. It does
     * nothing at all until the target is met, rather than quietly counting as a
     * sample: a button whose meaning changes partway through teaches the user
     * the wrong reflex for the one press that creates a wallet.
     *
     * This costs nothing in entropy. What the pool harvests is the timing
     * jitter between presses, so two collecting buttons gather exactly what
     * four would - and the seed is full strength either way, because
     * entropy_mix_pool() hashes this pool together with the hardware RNG and
     * user input can only add to it. */
    if (btn == BUTTON_ACCEPT) {
        if (entropy_target_met()) {
            screen_entropy_proceed();
        }
        return;
    }

    /* UP and DOWN collect. What is harvested is the timing, not which one. */
    entropy_add_user_event((uint8_t)btn, (uint64_t)esp_timer_get_time());
    ui_invalidate();
}

/* ============================================================================
 * Wipe Confirmation Screen
 *
 * Wiping was previously immediate from a three-item menu, and a live test
 * destroyed a wallet by selecting it while scrolling. Destructive and
 * irreversible actions get a stop.
 * ============================================================================ */

/* Deliberately awkward: hold-to-confirm rather than a single press, so the
 * gesture cannot be reached by the same reflex that selected the menu item. */
#define WIPE_CONFIRM_PRESSES 3

/* More friction when a seed has never been read back, because that is the case
 * where the device holds the only usable copy. */
#define WIPE_CONFIRM_PRESSES_UNVERIFIED 5

static int wipe_confirm_count = 0;

static void screen_wipe_confirm_enter(void)
{
    ESP_LOGI(TAG, "Wipe confirmation screen");
    wipe_confirm_count = 0;
}

static void screen_wipe_confirm_render(void)
{
    oled_clear();
    oled_draw_string_centered(0, "!! WIPE DEVICE !!");

    WalletStatus status = wallet_get_status();
    char line[22];
    snprintf(line, sizeof(line), "Erases %u wallet%s",
             (unsigned)status.wallet_count, status.wallet_count == 1 ? "" : "s");
    oled_draw_string_centered(2, line);
    oled_draw_string_centered(3, "and the PIN.");

    /* The dangerous case is not "was this the owner" - Settings already
     * required the PIN, and three wrong PINs wipe the device anyway. It is
     * erasing a seed whose backup was never confirmed correct. */
    uint8_t unverified = wallet_unverified_count();
    if (unverified > 0) {
        snprintf(line, sizeof(line), "%u NOT backed up!", (unsigned)unverified);
        oled_draw_string_centered(4, line);
    } else {
        oled_draw_string_centered(4, "No undo.");
    }

    int required = (wallet_unverified_count() > 0)
                       ? WIPE_CONFIRM_PRESSES_UNVERIFIED
                       : WIPE_CONFIRM_PRESSES;
    int left = required - wipe_confirm_count;
    char msg[32];
    snprintf(msg, sizeof(msg), "Press OK %u more", (unsigned)(left < 0 ? 0 : left));
    oled_draw_string_centered(6, msg);

    oled_draw_string(7, 0, HINT_WIPE);
}

static void screen_wipe_confirm_on_button(button_id_t btn)
{
    int required = (wallet_unverified_count() > 0)
                       ? WIPE_CONFIRM_PRESSES_UNVERIFIED
                       : WIPE_CONFIRM_PRESSES;

    if (btn == BUTTON_ACCEPT) {
        if (++wipe_confirm_count >= required) {
            ESP_LOGW(TAG, "Wipe confirmed by user");
            device_wipe();
            wallet_init();
            /* Same reasoning one level down (T45): the account selection lives
             * in the settings namespace, which a wipe does not erase, so a
             * wiped device would come up deriving the next seed at whatever
             * account the previous owner left selected. */
            hd_account_set(0);
            memzero(mnemonic_buffer, sizeof(mnemonic_buffer));
            mnemonic_word_count = 0;
            /* bip39.c's static phrase buffer as well. A wipe that leaves the
             * seed readable in RAM is not a wipe, and this path does not exit
             * through forget_mnemonic_unless_needed(). */
            mnemonic_clear();
            mnemonic_entry_clear(&entry);
            entropy_reset_user_pool();
            ESP_LOGW(TAG, "Device wiped");
            ui_set_screen(SCREEN_PIN_SETUP);
            return;
        }
    } else {
        /* Anything else aborts. */
        ui_set_screen(SCREEN_SETTINGS);
        return;
    }

    ui_invalidate();
}

/* ============================================================================
 * Mnemonic Verification Screen
 *
 * After showing a new seed, ask for a few words back. This is the standard
 * hardware-wallet flow and it exists because "I wrote it down" and "I wrote it
 * down correctly" are different claims, and the difference is only discovered
 * when the backup is needed.
 *
 * Reuses the predictive entry from mnemonic-entry.c, so verification feels the
 * same as import and exercises the same code path.
 * ============================================================================ */

#define VERIFY_CHALLENGES 3

static bool verify_done = false;
static int  verify_indices[VERIFY_CHALLENGES];
static int  verify_current = 0;
static int  verify_failures = 0;
static bool verify_last_wrong = false;

static void verify_pick_challenges(void)
{
    /* Distinct word positions, drawn from the hardware RNG rather than a
     * counter so the challenge cannot be anticipated. */
    for (int i = 0; i < VERIFY_CHALLENGES; i++) {
        bool unique;
        int candidate;
        do {
            unique = true;
            candidate = (int)(esp_random() % (uint32_t)mnemonic_word_count);
            for (int j = 0; j < i; j++) {
                if (verify_indices[j] == candidate) { unique = false; break; }
            }
        } while (!unique);
        verify_indices[i] = candidate;
    }
}

static void screen_mnemonic_verify_enter(void)
{
    ESP_LOGI(TAG, "Mnemonic verification screen");

    if (mnemonic_word_count == 0) {
        ui_set_screen(SCREEN_WALLET_INFO);
        return;
    }

    verify_pick_challenges();
    verify_current = 0;
    verify_failures = 0;
    verify_last_wrong = false;
    verify_done = false;
    mnemonic_entry_reset(&entry, 12);
}

static void screen_mnemonic_verify_render(void)
{
    oled_clear();

    if (verify_done) {
        oled_draw_string_centered(1, "Backup verified");
        oled_draw_string_centered(3, "Keep the phrase");
        oled_draw_string_centered(4, "somewhere safe.");
        oled_draw_string(7, 0, HINT_DONE);
        return;
    }

    WalletStatus vstatus = wallet_get_status();
    char header[22];
    unsigned step = (unsigned)(verify_current + 1) & 0x7;
    if (vstatus.wallet_count > 1) {
        snprintf(header, sizeof(header), "Verify W%u %u/%u",
                 (unsigned)vstatus.active_wallet_index & 0x3F,
                 step, (unsigned)VERIFY_CHALLENGES);
    } else {
        snprintf(header, sizeof(header), "Verify %u/%u", step,
                 (unsigned)VERIFY_CHALLENGES);
    }
    oled_draw_string_centered(0, header);

    char prompt[22];
    int word_no = verify_indices[verify_current] + 1;
    snprintf(prompt, sizeof(prompt), "Enter word #%d",
             (word_no < 1 || word_no > 24) ? 1 : word_no);
    oled_draw_string_centered(1, prompt);

    char typed[MNEMONIC_ENTRY_WORD_LEN + 24];
    entry_selector_line(typed, sizeof(typed));
    oled_draw_string(3, 0, "Type:");
    oled_draw_string(3, 36, typed);

    if (verify_last_wrong) {
        oled_draw_string(5, 0, "Wrong - try again");
    } else {
        if (entry.word_mode) {
            char line[22];
            snprintf(line, sizeof(line), "%d of %d",
                     entry.option_index + 1, entry.option_count);
            oled_draw_string(5, 0, "Choice:");
            oled_draw_string(5, 48, line);
        } else {
            const char *suggestion = mnemonic_entry_suggestion(&entry);
            if (suggestion) {
                oled_draw_string(5, 0, "Match:");
                oled_draw_string(5, 42, suggestion);
            }
        }
    }

    /* With a block open, BACK closes it instead of deleting a character, so
     * the hint has to say which one the next press will do. */
    oled_draw_string(7, 0, entry.in_group ? HINT_WORD_GROUP : HINT_SCROLL_DEL_SEL);
}

static void screen_mnemonic_verify_on_button(button_id_t btn)
{
    if (verify_done) {
        /* Any button continues; the device stays unlocked, since verifying a
         * backup is not a reason to make the user authenticate again. */
        ui_set_screen(SCREEN_WALLET_INFO);
        return;
    }

    verify_last_wrong = false;

    switch (btn) {
        case BUTTON_UP:   mnemonic_entry_scroll(&entry, -1); break;   /* PICKER_FORWARD */
        case BUTTON_DOWN: mnemonic_entry_scroll(&entry, 1);  break;

        case BUTTON_CANCEL:
            if (!mnemonic_entry_back(&entry)) {
                /* Backing out of verification returns to the seed, not onward -
                 * the user may need to read it again. */
                ui_set_screen(SCREEN_MNEMONIC_DISPLAY);
            }
            break;

        case BUTTON_ACCEPT: {
            if (mnemonic_entry_accept(&entry) == MNEMONIC_ENTRY_CONTINUE) {
                break;
            }

            /* A word was committed - compare it against the real one. */
            char expected[MNEMONIC_ENTRY_WORD_LEN];
            if (!get_mnemonic_word(mnemonic_buffer, verify_indices[verify_current],
                                   expected, sizeof(expected))) {
                ui_set_screen(SCREEN_WALLET_INFO);
                return;
            }

            bool correct = (strcmp(entry.words[0], expected) == 0);
            memzero(expected, sizeof(expected));
            mnemonic_entry_reset(&entry, 12);

            if (!correct) {
                verify_failures++;
                verify_last_wrong = true;
                ESP_LOGW(TAG, "Verification failed for word #%d (attempt %d)",
                         verify_indices[verify_current] + 1, verify_failures);

                /* Three misses means the backup is probably wrong, not the
                 * typing. Send them back to read the phrase again. */
                if (verify_failures >= 3) {
                    ESP_LOGW(TAG, "Too many misses; showing the phrase again");
                    verify_failures = 0;
                    ui_set_screen(SCREEN_MNEMONIC_DISPLAY);
                }
                break;
            }

            verify_current++;
            if (verify_current >= VERIFY_CHALLENGES) {
                ESP_LOGI(TAG, "Seed phrase verified");
                {
                    WalletStatus vs = wallet_get_status();
                    wallet_mark_backup_verified(vs.active_wallet_index);
                }
                /* The seed has served its purpose on screen; do not leave it
                 * sitting in .bss (AUDIT.md S5). */
                memzero(mnemonic_buffer, sizeof(mnemonic_buffer));
                mnemonic_word_count = 0;
                mnemonic_entry_clear(&entry);

                /* Say it passed before moving on. Jumping straight to the
                 * address screen leaves the user unsure whether the check
                 * succeeded or the device simply gave up on them. */
                verify_done = true;
                ui_invalidate();
                return;
            }
            break;
        }

        default:
            break;
    }

    ui_invalidate();
}

/* ============================================================================
 * Session Confirmation Screen
 *
 * The whole point of the passkey. It is derived from the ECDH shared secret,
 * so an attacker relaying between the host and the device holds a different
 * secret and cannot make the two codes agree. Encryption alone would protect a
 * conversation with an impostor perfectly well; a human comparing two screens
 * is what notices one.
 *
 * Which means this screen is not a formality to be skipped or auto-confirmed.
 * Without someone actually looking, the channel is encrypted and unauthenticated.
 * ============================================================================ */

static volatile bool session_confirm_pending = false;
static screen_id_t   session_confirm_return = SCREEN_MAIN_MENU;

static volatile bool host_unlock_pending = false;
static volatile bool host_lock_pending = false;

void ui_request_unlock(void)
{
    host_unlock_pending = true;
}

void ui_request_lock(void)
{
    host_lock_pending = true;
}

void ui_request_session_confirm(void)
{
    /* Called from the protocol task. Only sets a flag; the UI task owns screen
     * transitions, and having two tasks drive the screen graph is how you get
     * a render against half-changed state. */
    session_confirm_pending = true;
}

static void screen_session_confirm_enter(void)
{
    ESP_LOGI(TAG, "Session confirmation screen");
}

static void screen_session_confirm_render(void)
{
    oled_clear();
    oled_draw_string_centered(0, "Connect?");

    const char *code = session_passkey();
    char spaced[16];
    /* Grouped 3+3: six digits in a row are easy to misread, and misreading is
     * the failure this screen exists to prevent. */
    snprintf(spaced, sizeof(spaced), "%.3s %.3s", code, code + 3);
    oled_draw_string_centered(2, spaced);

    oled_draw_string_centered(4, "Match the code");
    oled_draw_string_centered(5, "shown in the app");

    oled_draw_string(7, 0, HINT_SESSION);
}

static void screen_session_confirm_on_button(button_id_t btn)
{
    if (btn == BUTTON_ACCEPT) {
        session_confirm();
        ESP_LOGI(TAG, "Session confirmed by user");
    } else {
        session_reset();
        ESP_LOGW(TAG, "Session denied by user");
    }
    ui_set_screen(session_confirm_return);
}

/* ============================================================================
 * Passphrase Entry
 *
 * BIP39's optional extra word. Applied to the active seed for this session
 * only; nothing about it is ever written down by the device, which is what
 * gives a passphrase wallet its deniability - and also why a typo cannot be
 * reported as an error. See docs/VAULT.md.
 * ============================================================================ */

static TextEntry passphrase_entry;

static void screen_passphrase_enter(void)
{
    ESP_LOGI(TAG, "Passphrase entry screen");
    text_entry_reset(&passphrase_entry);
}

/* Every button path off this screen already clears the buffer. This is for the
 * paths that are not button paths: the auto-lock timer, and a host request
 * that moves the screen out from under a half-typed passphrase. Without it a
 * passphrase the user never finished typing outlives the lock in .bss, which
 * is the S5 residue in the one buffer S5 did not cover. Unconditional - unlike
 * the seed buffer there is no hand-off that needs it kept. */
static void forget_passphrase_entry(screen_id_t next)
{
    (void)next;
    text_entry_clear(&passphrase_entry);
}

static void screen_passphrase_render(void)
{
    oled_clear();
    oled_draw_string_centered(0, "Passphrase");

    /* Show the tail of what has been typed. Masking it would be worse than
     * useless here: the user cannot verify a string they cannot see, and the
     * screen is already in their hand. */
    const char *text = passphrase_entry.text;
    int len = passphrase_entry.length;
    const char *tail = (len > 18) ? text + (len - 18) : text;
    char shown[24];
    /* Copy explicitly rather than through %s. The compiler cannot prove the
     * tail is short, and a truncating snprintf would silently misreport what
     * the user typed - on this screen that is the difference between two
     * wallets. */
    size_t out = 0;
    if (len > 18) {
        shown[out++] = '<';
    }
    for (size_t i = 0; tail[i] != '\0' && out < sizeof(shown) - 1; i++) {
        shown[out++] = tail[i];
    }
    shown[out] = '\0';
    oled_draw_string(2, 0, len ? shown : "(empty = no pass)");

    /* Neighbours, so the mode entries are visible before they are reached.
     * With the block selector on these are whole blocks ("a-f"), because a
     * press moves a block and the screen must say what a press does. */
    char prev[8], cur[8], next[8];
    text_entry_label_offset(&passphrase_entry, -1, prev, sizeof(prev));
    text_entry_label_offset(&passphrase_entry,  0, cur,  sizeof(cur));
    text_entry_label_offset(&passphrase_entry,  1, next, sizeof(next));

    char sel[32];
    snprintf(sel, sizeof(sel), "%s <%s> %s", prev, cur, next);
    oled_draw_string_centered(4, sel);

    char count[22];
    snprintf(count, sizeof(count), "%u chars", (unsigned)len & 0x7F);
    oled_draw_string_centered(5, count);

    /* ACCEPT opens a block before it picks anything, so it must not say SEL. */
    oled_draw_string(7, 0, text_entry_hint(&passphrase_entry));
}

static void screen_passphrase_on_button(button_id_t btn)
{
    switch (btn) {
        case BUTTON_UP:   text_entry_scroll(&passphrase_entry, -1); break;   /* PICKER_FORWARD */
        case BUTTON_DOWN: text_entry_scroll(&passphrase_entry, 1);  break;

        case BUTTON_CANCEL:
            /* Closes an open block first, and only then deletes: backing out
             * of the wrong block must not cost a character the user did type. */
            if (!text_entry_back(&passphrase_entry)) {
                text_entry_clear(&passphrase_entry);
                ui_set_screen(SCREEN_SETTINGS);
                return;
            }
            break;

        case BUTTON_ACCEPT: {
            TextEntryResult r = text_entry_accept(&passphrase_entry);
            if (r == TEXT_ENTRY_CANCELLED) {
                text_entry_clear(&passphrase_entry);
                ui_set_screen(SCREEN_SETTINGS);
                return;
            }
            if (r == TEXT_ENTRY_DONE) {
                if (!ensure_wallet_unlocked()) {
                    text_entry_clear(&passphrase_entry);
                    ui_set_screen(SCREEN_MAIN_MENU);
                    return;
                }
                wallet_set_passphrase(passphrase_entry.text,
                                      (size_t)passphrase_entry.length);
                /* Typed here, so it is not the host's to revoke: a disconnect
                 * must not drop the user back into the base wallet without
                 * saying so (T42). */
                protocol_note_device_passphrase();
                text_entry_clear(&passphrase_entry);
                ui_set_screen(SCREEN_PASSPHRASE_CONFIRM);
                return;
            }
            break;
        }

        default:
            break;
    }

    ui_invalidate();
}

/* ============================================================================
 * BLE Device Name (T56)
 *
 * "LeekWallet" broadcast to every scanner in range tells a room that someone
 * in it is carrying a hardware wallet. This is where that stops being true.
 *
 * Set here and nowhere else. There is deliberately no protocol method for it:
 * a host that could rename the device could make it advertise as something
 * else entirely, and the name is one of the few things a user can check
 * against their own phone.
 * ============================================================================ */

static TextEntry ble_name_entry;
/* Set when a name was refused, so the screen can say so rather than appearing
 * to ignore the press. Cleared on the next edit. */
static bool ble_name_rejected = false;

static void screen_ble_name_enter(void)
{
    ESP_LOGI(TAG, "BLE name screen");
    text_entry_reset(&ble_name_entry);
    ble_name_rejected = false;
}

static void screen_ble_name_render(void)
{
    oled_clear();
    oled_draw_string_centered(0, "BLE Name");

    /* The tail of what has been typed, same as the passphrase screen and for
     * the same reason: a name the user cannot see is a name they cannot
     * check. Copied by hand rather than through %s because -Werror=
     * format-truncation is on and a truncating snprintf would misreport what
     * was typed. */
    const char *text = ble_name_entry.text;
    int len = ble_name_entry.length;
    const char *tail = (len > 18) ? text + (len - 18) : text;
    char shown[24];
    size_t out = 0;
    if (len > 18) {
        shown[out++] = '<';
    }
    for (size_t i = 0; tail[i] != '\0' && out < sizeof(shown) - 1; i++) {
        shown[out++] = tail[i];
    }
    shown[out] = '\0';
    oled_draw_string(2, 0, len ? shown : ble_name_get());

    int n = text_entry_option_count(&ble_name_entry);
    int idx = ble_name_entry.option_index;
    char sa[4], sb[4], sc[4];
    const char *prev = text_entry_option_label(
        text_entry_option_at(&ble_name_entry, ((idx - 1) % n + n) % n), sa, sizeof(sa));
    const char *cur = text_entry_option_label(
        text_entry_option_at(&ble_name_entry, idx), sb, sizeof(sb));
    const char *next = text_entry_option_label(
        text_entry_option_at(&ble_name_entry, (idx + 1) % n), sc, sizeof(sc));

    char sel[22];
    snprintf(sel, sizeof(sel), "%s <%s> %s", prev, cur, next);
    oled_draw_string_centered(4, sel);

    if (ble_name_rejected) {
        /* Says the limit rather than "invalid": the user has to be able to act
         * on it, and the alternative to saying no here is a device that stops
         * advertising with no explanation at all. */
        char why[22];
        snprintf(why, sizeof(why), "Max %d chars", BLE_NAME_MAX_LEN);
        oled_draw_string_centered(5, why);
    } else {
        char count[22];
        snprintf(count, sizeof(count), "%u/%u chars",
                 (unsigned)len & 0x7F, (unsigned)BLE_NAME_MAX_LEN);
        oled_draw_string_centered(5, count);
    }

    oled_draw_string(7, 0, text_entry_on_group(&ble_name_entry) ? HINT_BLE_NAME_OPEN
                                                                : HINT_TEXT_SEL);
}

static void screen_ble_name_on_button(button_id_t btn)
{
    switch (btn) {
        case BUTTON_UP:   text_entry_scroll(&ble_name_entry, -1); break;   /* PICKER_FORWARD */
        case BUTTON_DOWN: text_entry_scroll(&ble_name_entry, 1);  break;

        case BUTTON_CANCEL:
            ble_name_rejected = false;
            if (!text_entry_backspace(&ble_name_entry)) {
                text_entry_clear(&ble_name_entry);
                ui_set_screen(SCREEN_SETTINGS);
                return;
            }
            break;

        case BUTTON_ACCEPT: {
            ble_name_rejected = false;
            TextEntryResult r = text_entry_accept(&ble_name_entry);
            if (r == TEXT_ENTRY_CANCELLED) {
                text_entry_clear(&ble_name_entry);
                ui_set_screen(SCREEN_SETTINGS);
                return;
            }
            if (r == TEXT_ENTRY_DONE) {
                /* An empty entry means "leave it alone", not "clear it": a
                 * nameless device is not something the user can ask for by
                 * pressing OK on a blank field by accident. */
                if (ble_name_entry.length == 0) {
                    ui_set_screen(SCREEN_SETTINGS);
                    return;
                }
                /* The refusal that keeps the radio alive. TEXT_ENTRY_MAX is
                 * 64 and the scan response holds 29, so this screen CAN
                 * produce a name that would stop advertising - and the answer
                 * is to say no and stay put, not to silently store a prefix of
                 * a name the user would never see again. */
                if (!ble_name_set(ble_name_entry.text)) {
                    ble_name_rejected = true;
                    break;
                }
                ESP_LOGI(TAG, "BLE name set (%d chars)", ble_name_entry.length);
                /* Put it on air now if the radio is up; otherwise the next
                 * start reads it. */
                ble_transport_refresh_name();
                text_entry_clear(&ble_name_entry);
                ui_set_screen(SCREEN_SETTINGS);
                return;
            }
            break;
        }

        default:
            break;
    }

    ui_invalidate();
}

/* ============================================================================
 * Passphrase Confirmation
 *
 * The one defence against a mistyped passphrase. A wrong passphrase does not
 * error - it derives a different, perfectly valid, empty-looking wallet - so
 * the device shows the resulting seed fingerprint and the user compares it
 * with what they recorded last time.
 *
 * Its limitation is worth stating: on first use there is nothing to compare
 * against. That is exactly why this fingerprint must be written down alongside
 * the seed, not merely glanced at.
 * ============================================================================ */

static void screen_passphrase_confirm_enter(void)
{
    ESP_LOGI(TAG, "Passphrase confirmation screen");
    /* Derivation runs PBKDF2 over the new passphrase, so this is the slow one. */
    wallet_info_refresh();
}

static void screen_passphrase_confirm_render(void)
{
    oled_clear();
    oled_draw_string_centered(0, wallet_has_passphrase() ? "Passphrase set"
                                                         : "No passphrase");

    /* This screen asks the user to confirm a passphrase by recognising its
     * fingerprint. With none there is nothing to recognise, and rendering a
     * blank row under "Match your record" invites them to accept a wallet
     * that never derived. */
    if (master_xfp[0] == '\0') {
        oled_draw_string_centered(2, "Error");
        oled_draw_string_centered(4, "No fingerprint");
        oled_draw_string_centered(6, "Cannot confirm");
        oled_draw_string(7, 0, HINT_RETRY);
        return;
    }

    /* The fingerprint names the seed, so it is the fact that actually says
     * "this is the passphrase you meant". */
    draw_master_xfp(2, "XFP");
    draw_master_zfp(4);

    oled_draw_string_centered(6, "Match your record");
    oled_draw_string(7, 0, HINT_RETRY_OK);
}

static void screen_passphrase_confirm_on_button(button_id_t btn)
{
    if (btn == BUTTON_CANCEL) {
        /* Wrong fingerprint means the wrong passphrase. Clear it rather than
         * leaving a wallet the user did not intend selected. */
        wallet_clear_passphrase();
        ui_set_screen(SCREEN_PASSPHRASE);
        return;
    }
    ui_set_screen(SCREEN_WALLET_INFO);
}

/* ============================================================================
 * Approval outcome
 *
 * The channel a host request waits on while the user answers a question on
 * the device. LeekWallet carried transaction, message and typed-data
 * review pages here; they are gone (roadmap 1.1), and the host-supplied
 * passphrase is the one question left. The Zcash transaction review of a later
 * phase rides the same outcome rather than a second one.
 * ============================================================================ */

static volatile SignOutcome sign_outcome = SIGN_PENDING;

#if LEEK_HAS_ZCASH
/* A Zcash review requested by the protocol task, not yet on screen. Here
 * rather than with its screen because ui_user_is_answering() reads it. */
static volatile bool zsign_review_pending = false;
/* zShowAddress: the address-check screen is wanted (see SCREEN_ADDRESS_CHECK). */
static volatile bool addr_check_pending = false;
#endif

/* Raised when the approval window closed with nobody answering, so the UI
 * task takes the question down. */
static volatile bool sign_expired_pending = false;

SignOutcome ui_sign_outcome(void)
{
    return sign_outcome;
}

/**
 * The approval window closed with nobody answering. Take the question down.
 *
 * wait_for_user() has a 120 s deadline, after which it answers the host and
 * calls ui_sign_clear() -- which resets the outcome but leaves the screen
 * exactly where it was. A question left on the glass after the host has been
 * told no would still accept buttons, and the device must never appear to
 * accept something nothing is waiting for.
 *
 * Sets a flag rather than moving the screen: screen transitions belong to the
 * UI task, and this runs on the protocol task.
 */
void ui_sign_expire(void)
{
    sign_expired_pending = true;
    ui_invalidate();
}

void ui_sign_clear(void)
{
    sign_outcome = SIGN_PENDING;
}

/* ============================================================================
 * Host-supplied Passphrase Confirmation (PROTOCOL.md 5)
 *
 * The app can act as a keyboard for the passphrase, which is a real
 * convenience and a real downgrade: a compromised host sees the passphrase
 * before encryption ever touches it. What makes the trade survivable is this
 * screen. A wrong or substituted passphrase derives a different, perfectly
 * valid wallet rather than failing, so the fingerprint shown here is the only
 * signal that anything went wrong — and rejecting it must put the device back
 * where it was, not leave a wallet nobody chose selected.
 *
 * Separate from SCREEN_PASSPHRASE_CONFIRM (the on-device entry path) for two
 * reasons: it answers a waiting host through the sign-outcome channel, and it
 * has to say out loud that the passphrase came from the host. Sharing a screen
 * would mean the weaker path borrowing the stronger one's wording.
 * ============================================================================ */

static volatile bool host_passphrase_pending = false;
/* The fingerprint the protocol task derived after applying the passphrase, as
 * eight hex characters, or "" for none. Derived there rather than here for the
 * T47 reason: the screen must name what the device actually did. */
static char host_passphrase_xfp[9];

void ui_request_passphrase_confirm(uint32_t fingerprint)
{
    snprintf(host_passphrase_xfp, sizeof(host_passphrase_xfp), "%08lX",
             (unsigned long)fingerprint);
    sign_outcome = SIGN_PENDING;
    host_passphrase_pending = true;
}

/* M-2. Placed here rather than beside ui_request_session_confirm() only
 * because it reads two flags that are defined this far down the file. */
bool ui_user_is_answering(void)
{
    /* A request already handed to the UI task counts even before the screen
     * has changed: these flags are set by the protocol task and acted on a
     * tick later, and a `hello` landing in that gap would discard the pending
     * approval just as thoroughly as one landing a tick after it.
     *
     * SCREEN_SESSION_CONFIRM is deliberately absent. A second handshake while
     * the pairing screen is up already resets and repaints, so the user
     * compares the new digits rather than the old — that behaviour is tested
     * and it is the right one. Refusing here instead would strand a host that
     * abandoned a handshake behind a screen only a button press can clear. */
    if (host_passphrase_pending) {
        return true;
    }
#if LEEK_HAS_ZCASH
    if (zsign_review_pending || addr_check_pending) {
        return true;
    }
#endif

    switch (ui_get_screen()) {
        case SCREEN_HOST_PASSPHRASE_CONFIRM:
        case SCREEN_ZSIGN_REVIEW:
        case SCREEN_ADDRESS_CHECK:
        case SCREEN_PASSPHRASE_CONFIRM:
        case SCREEN_WIPE_CONFIRM:
        /* Not questions, but a seed is on the glass and the user is working
         * through it. Wiping that away for a stranger's pairing prompt loses
         * their place in the one procedure that has no second chance. */
        case SCREEN_MNEMONIC_DISPLAY:
        case SCREEN_MNEMONIC_VERIFY:
            return true;
        default:
            return false;
    }
}

static void screen_host_passphrase_enter(void)
{
    ESP_LOGI(TAG, "Host passphrase confirmation screen");
}

static void screen_host_passphrase_render(void)
{
    oled_clear();
    oled_draw_string_centered(0, "App passphrase");

    if (strlen(host_passphrase_xfp) != 8) {
        /* Nothing to recognise means nothing to confirm. */
        oled_draw_string_centered(3, "No fingerprint");
        oled_draw_string(7, 0, HINT_NO);
        return;
    }

    char line[22];
    snprintf(line, sizeof(line), "XFP %s", host_passphrase_xfp);
    oled_draw_string_centered(2, line);
    oled_draw_string(4, 0, "Typed on host!");
    oled_draw_string(6, 0, "Match your record");
    oled_draw_string(7, 0, HINT_NO_YES);
}

static void screen_host_passphrase_on_button(button_id_t btn)
{
    if (btn == BUTTON_ACCEPT && strlen(host_passphrase_xfp) == 8) {
        sign_outcome = SIGN_APPROVED;
    } else if (btn == BUTTON_ACCEPT || btn == BUTTON_CANCEL) {
        /* The protocol task clears the passphrase on a rejection: the wrong
         * wallet must not stay selected just because the user said no. */
        sign_outcome = SIGN_REJECTED;
    } else {
        return;
    }
    memzero(host_passphrase_xfp, sizeof(host_passphrase_xfp));
    ui_set_screen(SCREEN_WALLET_INFO);
}

static const screen_t screen_host_passphrase = {
    .enter = screen_host_passphrase_enter,
    .render = screen_host_passphrase_render,
    .on_button = screen_host_passphrase_on_button,
    .exit = NULL,
};

/* ============================================================================
 * Zcash Transaction Review (roadmap 3.4)
 *
 * A host asked the device to sign. The summary was produced by the signer
 * backends on the device (both of them, agreeing, by default), never taken
 * from the host. Every page comes from zcash-review.c, so the host suite
 * tests the exact text drawn here.
 *
 * ACCEPT moves forward and signs only on the last page, so the only way to
 * a signature is through every page. CANCEL rejects from any page.
 * ============================================================================ */

#if LEEK_HAS_ZCASH

void ui_request_zsign_review(const zs_summary *s, uint8_t network)
{
    zsign_summary = s;
    zsign_network = network;
    zsign_from_sd = false;
    zsign_from_qr = false;
    sign_outcome = SIGN_PENDING;
    zsign_review_pending = true;
}

static void screen_zsign_review_enter(void)
{
    ESP_LOGI(TAG, "Zcash transaction review");
    zsign_page = 0;
}

static void screen_zsign_review_render(void)
{
    oled_clear();
    ZReviewPage page;
    if (!zsign_summary || !zreview_render(zsign_summary, zsign_network, zsign_page, &page)) {
        oled_draw_string_centered(3, "Nothing to sign");
        oled_draw_string(7, 0, HINT_BACK);
        return;
    }
    for (int r = 0; r < ZREVIEW_ROWS; r++) {
        oled_draw_string(r, 0, page.row[r]);
    }
}

static void screen_zsign_review_on_button(button_id_t btn)
{
    int last = zsign_summary ? zreview_page_count(zsign_summary, zsign_network) - 1 : -1;
    switch (btn) {
        case BUTTON_UP:
            if (zsign_page > 0) zsign_page--;
            break;
        case BUTTON_DOWN:
            if (zsign_page < last) zsign_page++;
            break;
        case BUTTON_ACCEPT:
            if (last >= 0 && zsign_page < last) {
                zsign_page++;
                break;
            }
            zsign_summary = NULL;
            if (zsign_from_qr) {
                qr_sign_review_done(last >= 0);
                return;
            }
#if UI_HAS_SD_SEED
            if (zsign_from_sd) {
                sd_sign_review_done(last >= 0);
                return;
            }
#endif
            sign_outcome = last >= 0 ? SIGN_APPROVED : SIGN_REJECTED;
            ui_set_screen(SCREEN_WALLET_INFO);
            return;
        case BUTTON_CANCEL:
            zsign_summary = NULL;
            if (zsign_from_qr) {
                qr_sign_review_done(false);
                return;
            }
#if UI_HAS_SD_SEED
            if (zsign_from_sd) {
                sd_sign_review_done(false);
                return;
            }
#endif
            sign_outcome = SIGN_REJECTED;
            ui_set_screen(SCREEN_WALLET_INFO);
            return;
        default:
            return;
    }
    ui_invalidate();
}

static const screen_t screen_zsign_review = {
    .enter = screen_zsign_review_enter,
    .render = screen_zsign_review_render,
    .on_button = screen_zsign_review_on_button,
    .exit = NULL,
};

/* ============================================================================
 * Address Check (zShowAddress, roadmap 1.7)
 *
 * The app shows an address and asks the device to show the same one. The
 * device derived it itself, from its own seed; the user pages through the
 * whole UA and answers on the last page. A mismatch is the one way to catch
 * a companion that would hand out an address the device cannot spend from.
 * ============================================================================ */

static ZcashAccount  addr_check;
static int           addr_check_page;

void ui_request_show_address(const char *ua, const char *t_address, uint32_t account,
                             uint8_t network)
{
    memset(&addr_check, 0, sizeof addr_check);
    snprintf(addr_check.keys.ua, sizeof addr_check.keys.ua, "%s", ua);
    snprintf(addr_check.t_address, sizeof addr_check.t_address, "%s", t_address);
    addr_check.account = account;
    addr_check.network = network;
    sign_outcome = SIGN_PENDING;
    addr_check_pending = true;
}

/* The UA's text pages, then the question. */
static int addr_check_pages(void)
{
    return zrecv_page_count(&addr_check) - 3 + 1;
}

static void screen_address_check_enter(void)
{
    addr_check_page = 0;
}

static void screen_address_check_render(void)
{
    oled_clear();
    const int last = addr_check_pages() - 1;
    ZRecvPage p;
    if (addr_check_page < last && zrecv_render(&addr_check, addr_check_page, &p)) {
        for (int r = 0; r < ZRECV_ROWS - 1; r++) {
            if (p.row[r][0]) oled_draw_string(r, 0, p.row[r]);
        }
        oled_draw_string(7, 0, HINT_ADDR_PAGE);
        return;
    }
    oled_draw_string_centered(0, "Address check");
    oled_draw_string_centered(2, "Is this the address");
    oled_draw_string_centered(3, "the app shows?");
    char line[22];
    snprintf(line, sizeof line, "acct %u  %s", (unsigned)(addr_check.account % 100000u),
             addr_check.network == ZCASH_NET_MAIN ? "MAIN" : "TEST");
    oled_draw_string_centered(5, line);
    oled_draw_string(7, 0, HINT_ADDR_ASK);
}

static void address_check_done(SignOutcome o)
{
    sign_outcome = o;
    memzero(&addr_check, sizeof addr_check);
    ui_set_screen(SCREEN_MAIN_MENU);
}

static void screen_address_check_on_button(button_id_t btn)
{
    const int last = addr_check_pages() - 1;
    switch (btn) {
        case BUTTON_UP:
            if (addr_check_page > 0) addr_check_page--;
            break;
        case BUTTON_DOWN:
            if (addr_check_page < last) addr_check_page++;
            break;
        case BUTTON_ACCEPT:
            /* YES only on the question page: every page of the address is
             * passed on the way there. */
            if (addr_check_page < last) {
                addr_check_page++;
                break;
            }
            address_check_done(SIGN_APPROVED);
            return;
        case BUTTON_CANCEL:
            address_check_done(SIGN_REJECTED);
            return;
        default:
            return;
    }
    ui_invalidate();
}

static const screen_t screen_address_check = {
    .enter = screen_address_check_enter,
    .render = screen_address_check_render,
    .on_button = screen_address_check_on_button,
    .exit = NULL,
};
#endif /* LEEK_HAS_ZCASH */

/* ============================================================================
 * Temporary Seed Screen (T69)
 *
 * What SeedSigner does by construction, offered here as a session: the seed is
 * typed, held in RAM, and never written down by the device. Every at-rest
 * attack in docs/AUDIT-SECRETS.md - the flash dump, the PIN that falls to a GPU
 * in seconds, the logical erase that leaves ciphertext behind - is an attack on
 * a stored seed. This mode has nothing stored to attack.
 *
 * It buys that with two costs the user must read before they pay them, which
 * is why this screen exists between the menu and the keyboard:
 *
 *   The PIN protects nothing at rest. It still stops someone picking up an
 *   unlocked device, and that is genuinely all it does here - there is no
 *   ciphertext for it to guard, because there is no ciphertext.
 *
 *   A reboot, a crash or a flat battery loses the seed. The device cannot
 *   warn about that after the fact; the words are gone and only the user's own
 *   backup has them.
 *
 * Two pages rather than one dense screen: 128x64 fits about five readable rows,
 * and a warning nobody can read is decoration.
 * ============================================================================ */

#define TEMP_SEED_PAGES 2
static int temp_seed_page = 0;

static void screen_temp_seed_enter(void)
{
    ESP_LOGI(TAG, "Temporary seed warning");
    temp_seed_page = 0;
}

static void screen_temp_seed_render(void)
{
    oled_clear();
    oled_draw_string_centered(0, "-- Temp seed --");

    if (temp_seed_page == 0) {
        oled_draw_string(2, 0, "Seed is NOT saved.");
        oled_draw_string(3, 0, "Lock, reboot or");
        oled_draw_string(4, 0, "power loss erases");
        oled_draw_string(5, 0, "it. Keep a backup.");
        oled_draw_string(7, 0, HINT_TEMP_FIRST);
    } else {
        oled_draw_string(2, 0, "PIN guards nothing");
        oled_draw_string(3, 0, "at rest: nothing");
        oled_draw_string(4, 0, "is stored to guard.");
        oled_draw_string(5, 0, "Auto-lock 30 min.");
        oled_draw_string(7, 0, HINT_TEMP_LAST);
    }
}

static void screen_temp_seed_on_button(button_id_t btn)
{
    switch (btn) {
        case BUTTON_UP:
            if (temp_seed_page > 0) {
                temp_seed_page--;
            }
            break;

        case BUTTON_DOWN:
            if (temp_seed_page < TEMP_SEED_PAGES - 1) {
                temp_seed_page++;
            }
            break;

        case BUTTON_CANCEL:
            ui_set_screen(SCREEN_MAIN_MENU);
            return;

        case BUTTON_ACCEPT:
            /* Accepting from either page. The second page is the harsher one,
             * but forcing a scroll to reach the button teaches scrolling, not
             * reading, and the first page already carries the loss the user
             * cannot undo. */
            entry_is_temporary = true;
            ui_set_screen(SCREEN_MNEMONIC_ENTRY);
            return;

        default:
            break;
    }

    ui_invalidate();
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void ui_init(void)
{
    /* Register built-in screens */
    screens[SCREEN_BOOT] = &screen_boot;
    screens[SCREEN_PIN_SETUP] = &screen_pin_setup;
    screens[SCREEN_PIN_UNLOCK] = &screen_pin_unlock;
    screens[SCREEN_PIN_CHANGE] = &screen_pin_change;
    screens[SCREEN_MAIN_MENU] = &screen_main_menu;
    screens[SCREEN_WALLET_INFO] = &screen_wallet_info;
    screens[SCREEN_MNEMONIC_ENTRY] = &screen_mnemonic_entry;
    screens[SCREEN_WALLET_CREATE] = &screen_wallet_create;
    screens[SCREEN_WALLET_SELECT] = &screen_wallet_select;
    screens[SCREEN_MNEMONIC_DISPLAY] = &screen_mnemonic_display;
    screens[SCREEN_SETTINGS] = &screen_settings;
    screens[SCREEN_ENTROPY] = &screen_entropy;
    screens[SCREEN_WIPE_CONFIRM] = &screen_wipe_confirm;
    screens[SCREEN_MNEMONIC_VERIFY] = &screen_mnemonic_verify;
    screens[SCREEN_SESSION_CONFIRM] = &screen_session_confirm;
    screens[SCREEN_LOCK_HOLD] = &screen_lock_hold;
    screens[SCREEN_LOCK_CONFIRM] = &screen_lock_confirm;
    screens[SCREEN_SHOW_SEED_CONFIRM] = &screen_show_seed_confirm;
    screens[SCREEN_PASSPHRASE] = &screen_passphrase;
    screens[SCREEN_BLE_NAME] = &screen_ble_name;
    screens[SCREEN_TEMP_SEED] = &screen_temp_seed;
    screens[SCREEN_PASSPHRASE_CONFIRM] = &screen_passphrase_confirm;
    screens[SCREEN_HOST_PASSPHRASE_CONFIRM] = &screen_host_passphrase;
#if LEEK_HAS_ZCASH
    screens[SCREEN_ZSIGN_REVIEW] = &screen_zsign_review;
#endif
    screens[SCREEN_QR_OUT] = &screen_qr_out;
    screens[SCREEN_SHOW_ADDRESS] = &screen_show_address;
    screens[SCREEN_SD_SEED] = &screen_sd_seed;
#if LEEK_HAS_ZCASH
    screens[SCREEN_ADDRESS_CHECK] = &screen_address_check;
#endif
    screens[SCREEN_SCAN] = &screen_scan;

    current_screen = SCREEN_BOOT;
    needs_render = true;
    boot_splash_restart();

    settings_load();
    oled_set_contrast(BRIGHTNESS_LEVELS[brightness_choice]);

    ESP_LOGI(TAG, "UI initialized (auto-lock %s, brightness %s)",
             lock_timeout_label(lock_timeout_choice),
             brightness_label(brightness_choice));
}

void ui_set_screen(screen_id_t screen)
{
    if (screen >= SCREEN_COUNT) {
        ESP_LOGE(TAG, "Invalid screen ID: %d", screen);
        return;
    }

    /* Exit current screen. This is where secrets held in static buffers get
     * zeroed - see the .exit hooks and AUDIT S5. */
    if (screens[current_screen] && screens[current_screen]->exit) {
        screens[current_screen]->exit(screen);
    }

    current_screen = screen;

    /* Enter new screen */
    if (screens[current_screen] && screens[current_screen]->enter) {
        screens[current_screen]->enter();
    }

    needs_render = true;
    ESP_LOGI(TAG, "Screen changed to %d", screen);
}

screen_id_t ui_get_screen(void)
{
    return current_screen;
}

void ui_handle_button(button_id_t btn)
{
    /* Any press re-opens the BLE advertising window if it has lapsed.
     *
     * The radio stops advertising after two minutes with nobody connecting
     * (ble.c, BLE_ADV_WINDOW_MS) because it was the board's largest continuous
     * power draw and the device was uncomfortably warm to hold. The cost of
     * that is a device nobody can find, so the way back has to be something a
     * user would do anyway without being told -- pressing a button -- rather
     * than a menu they must first learn exists.
     *
     * It is deliberately not conditional on which button, and it does not
     * consume the press: the call is a no-op unless the window is actually
     * closed, so the button keeps whatever meaning the current screen gives
     * it. A press that both wakes the radio and does its normal job is the
     * behaviour somebody expects from a device that went quiet. */
    ble_transport_advertise_again();

    /* Any input, whatever takes it, is someone at the device: the unlock
     * screen's idle timer starts over. */
    pin_idle_since_us = esp_timer_get_time();

    /* The wrong-network notice takes the press that dismisses it: the
     * screen under it is not visible, so its buttons must not act. */
    if (wrong_network_until_us) {
        wrong_network_until_us = 0;
        ui_invalidate();
        return;
    }

    if (screens[current_screen] && screens[current_screen]->on_button) {
        screens[current_screen]->on_button(btn);
    }
}

void ui_render(void)
{
    if (wrong_network_until_us) {
        render_wrong_network();
    } else if (screens[current_screen] && screens[current_screen]->render) {
        screens[current_screen]->render();
    }
    /* Screens draw into the buffer; the panel changes exactly once, here. */
    oled_flush();
    needs_render = false;
}

void ui_invalidate(void)
{
    needs_render = true;
}

void ui_poll_deferred(void)
{
    if (create_generate_pending && current_screen == SCREEN_WALLET_CREATE) {
        wallet_create_run_generation();
        return;
    }
    /* Left the screen before the work ran (CANCEL is still live while
     * "Generating..." is up, because the handler no longer blocks). The
     * request goes with the screen. */
    create_generate_pending = false;

    /* The passphrase changed under a screen that is already showing a
     * fingerprint derived from the old one (T42). Every way that can happen is
     * another task or a timer: a host setPassphrase, a host-rejected
     * confirmation, an auto-lock. Re-derive rather than merely repaint - the
     * fingerprint itself is wrong now, not just the marker beside it. */
    if (current_screen == SCREEN_WALLET_INFO &&
        wallet_has_passphrase() != wallet_info_passphrase_shown) {
        ESP_LOGW(TAG, "Passphrase changed under the wallet screen; re-deriving");
        wallet_info_refresh();
        ui_invalidate();
    }
}

bool ui_needs_render(void)
{
    return needs_render;
}

void ui_clear_invalidation(void)
{
    needs_render = false;
}

void ui_register_screen(screen_id_t id, const screen_t *screen)
{
    if (id < SCREEN_COUNT) {
        screens[id] = screen;
    }
}

/* The host asked the device to lock (PROTOCOL.md). Extracted from the task
 * loop so the tests exercise the same path the firmware does - the bug this
 * whole family came from was four call sites disagreeing, and a test driving
 * its own copy of one of them would have hidden it. */
static void service_host_lock(void)
{
    if (!host_lock_pending) {
        return;
    }
    host_lock_pending = false;
    lock_device();
    ui_set_screen(SCREEN_PIN_UNLOCK);
}

/* Take down a confirmation nobody answered. Guarded on the screen still being
 * the one that was raised: by the time this runs the user may have walked
 * somewhere else entirely, and yanking them out of it to report a question
 * they already abandoned is its own annoyance. */
static void service_sign_expiry(void)
{
    if (!sign_expired_pending) {
        return;
    }
    sign_expired_pending = false;

#if LEEK_HAS_ZCASH
    if (ui_get_screen() == SCREEN_ZSIGN_REVIEW) {
        /* The protocol task has already refused the host and dropped the
         * upload; the summary this screen points at is gone with it. */
        zsign_summary = NULL;
        ui_set_screen(SCREEN_WALLET_INFO);
        return;
    }
    if (ui_get_screen() == SCREEN_ADDRESS_CHECK) {
        memzero(&addr_check, sizeof addr_check);
        ui_set_screen(SCREEN_MAIN_MENU);
        return;
    }
#endif
    if (ui_get_screen() == SCREEN_HOST_PASSPHRASE_CONFIRM) {
        /* The wallet is the honest place to land: whatever was proposed has
         * already been dropped by the protocol task. */
        ui_set_screen(SCREEN_WALLET_INFO);
    }
}

/* The two screens that change with nobody touching anything, by time alone:
 * the splash steps its loop (and blanks), and the unlock screen, left idle,
 * falls back to the splash. Nothing else: PIN setup and change, the review
 * screens and the rest keep waiting for the user. */
static void service_idle_screens(void)
{
    if (wrong_network_until_us) {
        return;   /* the notice is on top; nothing under it is visible */
    }
    const int64_t now_us = esp_timer_get_time();
    if (current_screen == SCREEN_BOOT) {
        if (!splash_view_equal(splash_loop_view(&boot_splash, now_us / 1000), boot_splash_shown)) {
            ui_invalidate();
        }
    } else if (current_screen == SCREEN_PIN_UNLOCK &&
               now_us - pin_idle_since_us >= UI_PIN_IDLE_SPLASH_US) {
        /* Through ui_set_screen, so forget_pin_entry zeroes the digits.
         * No pin_verify(): the counter is untouched either way. */
        ESP_LOGI(TAG, "PIN entry idle for %lld s; back to the splash",
                 (long long)(UI_PIN_IDLE_SPLASH_US / 1000000));
        ui_set_screen(SCREEN_BOOT);
    }
}

/* How long ui_task may sleep on the input queue: 100 ms normally, 1 ms while
 * scanning, and on the splash no longer than its next frame or blink. */
static TickType_t ui_wait_ticks(void)
{
    if (current_screen == SCREEN_SCAN) {
        return pdMS_TO_TICKS(1);
    }
    if (current_screen == SCREEN_BOOT) {
        const uint32_t ms = splash_loop_next_change_ms(&boot_splash, ui_now_ms());
        if (ms < 100) {
            return pdMS_TO_TICKS(ms);
        }
    }
    return pdMS_TO_TICKS(100);
}

void ui_task(void *pvParameters)
{
    (void)pvParameters;
    QueueHandle_t queue = button_get_queue();
    button_event_t event;

    /* Initial render: the boot screen, which is the looping splash. */
    ui_render();

    ESP_LOGI(TAG, "UI task started");

    lock_note_activity();

    while (1) {
        /* Wait for button event with timeout for periodic refresh. Not while
         * scanning: this wait came before every frame, so up to 100 ms of
         * each capture cycle was spent idle on top of the decode, and the
         * camera's newest frame went stale waiting for it. */
        const TickType_t wait = ui_wait_ticks();
        if (xQueueReceive(queue, &event, wait) == pdTRUE) {
            lock_note_activity();
            ui_handle_button(event.id);
        } else if (lock_check_timeout()) {
            /* Just locked; fall through to render the unlock screen. */
        }

        /* A handshake arrived on the protocol task. Interrupt whatever is on
         * screen: the host is waiting, and the user needs to compare a code. */
        if (session_confirm_pending) {
            session_confirm_pending = false;
            /* A second handshake
             * while this screen is already up derives a NEW passkey, and
             * without a repaint the user would compare the app's code against
             * the previous handshake's digits and see a mismatch that is not
             * one. ui_set_screen() covers the other branch. */
            ui_invalidate();
            if (ui_get_screen() != SCREEN_SESSION_CONFIRM) {
                /* Return somewhere useful. Coming back to the boot splash
                 * after approving a connection reads as the device having
                 * reset, and leaves the user pressing keys to get anywhere. */
                screen_id_t here = ui_get_screen();
                session_confirm_return =
                    (here == SCREEN_BOOT)
                        ? (pin_is_unlocked() ? SCREEN_MAIN_MENU : SCREEN_PIN_UNLOCK)
                        : here;
                ui_set_screen(SCREEN_SESSION_CONFIRM);
            }
        }

        service_lock_hold();
        service_idle_screens();
        service_qr_out();
        service_scan();

        /* A companion that died without saying so. Tears down the channel
         * only: the wallet stays unlocked and a temporary seed survives, so
         * this costs a re-handshake and never a retyped phrase. */
        if (session_check_idle()) {
            ESP_LOGI(TAG, "Session idle for %d s; channel closed",
                     SESSION_IDLE_TIMEOUT_S);
        }

        service_sign_expiry();
        service_wrong_network();

        if (host_passphrase_pending) {
            host_passphrase_pending = false;
            ui_set_screen(SCREEN_HOST_PASSPHRASE_CONFIRM);
        }
#if LEEK_HAS_ZCASH
        if (zsign_review_pending) {
            zsign_review_pending = false;
            ui_set_screen(SCREEN_ZSIGN_REVIEW);
        }
        if (addr_check_pending) {
            addr_check_pending = false;
            ui_set_screen(SCREEN_ADDRESS_CHECK);
        }
#endif

        if (host_unlock_pending) {
            host_unlock_pending = false;
            if (!pin_is_unlocked()) {
                pending_mnemonic_display = false;
                ui_set_screen(SCREEN_PIN_UNLOCK);
            }
        }

        service_host_lock();

        /* Re-render if needed */
        if (ui_needs_render()) {
            ui_render();
        }

        /* After the repaint, never before: the work deferred here is deferred
         * precisely so the frame announcing it is already on the panel
         * (AUDIT S8f). */
        ui_poll_deferred();
    }
}

/* ============================================================================
 * Test-only accessors (sim/test_ui.c)
 *
 * The interesting invariants here are about state that is deliberately private:
 * whether the seed buffer is zero after leaving the seed flow, and whether the
 * typed PIN survives a screen change. Both are unobservable from the outside,
 * which is why they went unnoticed in the first place. Same precedent as
 * pin__reset_static_state_for_test(): compiled out of the firmware entirely.
 * ============================================================================ */

#ifdef LEEK_HOST_TEST

const char *ui__mnemonic_buffer_for_test(void)     { return mnemonic_buffer; }
size_t      ui__mnemonic_buffer_size_for_test(void) { return sizeof(mnemonic_buffer); }
int         ui__mnemonic_word_count_for_test(void)  { return mnemonic_word_count; }

const char *ui__pin_entry_for_test(void)  { return pin_entry; }
int         ui__pin_cursor_for_test(void) { return pin_cursor; }
int         ui__pin_option_for_test(void) { return current_digit; }

/* The auto-lock decision, which otherwise only ui_task()'s loop can reach.
 *
 * Exposed rather than reimplemented in the test: "what the device drops when
 * it locks itself" is a T42 invariant, and a test asserting it against its own
 * copy of the rule would pass while the device kept a passphrase applied. */
bool ui__check_autolock_for_test(void) { return lock_check_timeout(); }
void ui__service_sign_expiry_for_test(void) { service_sign_expiry(); }
void ui__service_wrong_network_for_test(void) { service_wrong_network(); }
void ui__service_lock_hold_for_test(void) { service_lock_hold(); }
void ui__service_idle_screens_for_test(void) { service_idle_screens(); }
void ui__set_splash_ok_only_for_test(bool ok_only) { splash_ok_only = ok_only; }
bool ui__splash_blank_for_test(void) { return boot_splash_shown.blank; }
void ui__service_host_lock_for_test(void) { service_host_lock(); }
void ui__service_qr_out_for_test(void) { service_qr_out(); }
void ui__scan_feed_for_test(const char *ur) { scan_handle(ur, strlen(ur)); }
const char *ui__scan_status_for_test(void) { return scan_status; }

/* The cached fingerprint, which is not on any screen while the device is
 * locked and so cannot be asserted through the framebuffer - the stale-XFP
 * failure is that it is still HERE when the next screen goes to draw it. */
const char *ui__master_xfp_for_test(void) { return master_xfp; }
const char *ui__master_zfp_for_test(void) { return master_zfp; }

/* Which account the device is on, and whether the wallet screen's
 * fingerprint was derived with a passphrase applied. Both are private on purpose;
 * both are the state a stale-passphrase or wrong-account bug lives in. */
uint32_t ui__account_for_test(void)            { return hd_account; }
bool ui__wallet_info_pass_shown_for_test(void) { return wallet_info_passphrase_shown; }

const MnemonicEntry *ui__entry_for_test(void)  { return &entry; }
bool ui__entry_is_temporary_for_test(void)     { return entry_is_temporary; }

/* The timeout in force and the one in NVS. Separate on purpose (T69), and a
 * test that could only see one of them could not tell a temporary session's
 * longer default from a stored wallet's timeout having been lengthened. */
int ui__lock_timeout_choice_for_test(void)        { return lock_timeout_choice; }
int ui__lock_timeout_stored_choice_for_test(void) { return lock_timeout_stored_choice; }
bool ui__entry_choosing_length_for_test(void)  { return entry_choosing_length; }
int  ui__entry_length_choice_for_test(void)    { return entry_length_choice; }

/* Back to power-on state. Covers everything the tests observe; screens with
 * purely cosmetic state (entropy meter, QR page) reinitialise in enter(). */
void ui__reset_static_state_for_test(void)
{
    current_screen = SCREEN_BOOT;
    needs_render = true;

    menu_selection = 0;
    menu_item_count = 0;
    settings_selection = 0;
    wallet_list_selection = 0;

    hd_account = 0;
    wallet_info_passphrase_shown = false;
    memzero(wallet_info_error, sizeof(wallet_info_error));
    memzero(mnemonic_buffer, sizeof(mnemonic_buffer));
    mnemonic_word_count = 0;
    mnemonic_page = 0;
    pending_mnemonic_display = false;

    entropy_mode     = ENTROPY_MODE_CHOOSE;
    entropy_menu_sel = 0;
    dice_face        = 1;
    dice_last        = 0;

    create_word_count = 12;
    create_show_mnemonic = false;
    create_generate_pending = false;
    memzero(create_error, sizeof(create_error));

    mnemonic_entry_clear(&entry);
    memzero(entry_error, sizeof(entry_error));
    entry_choosing_length = true;
    entry_length_choice = 12;
    entry_is_temporary = false;
    temp_seed_page = 0;
    /* Both selectors back to the default, together - a test that flipped the
     * setting must not leak it into the next one. */
    entry_blocks_apply(false);
    text_entry_reset(&passphrase_entry);

    memzero(pin_entry, sizeof(pin_entry));
    memzero(pin_first_entry, sizeof(pin_first_entry));
    pin_cursor = 0;
    current_digit = 0;
    pin_confirm_mode = false;
    pin_change_phase = PIN_CHANGE_CURRENT;
    memzero(pin_change_current, sizeof(pin_change_current));
    memzero(pin_change_message, sizeof(pin_change_message));

    verify_done = false;
    verify_current = 0;
    verify_failures = 0;
    verify_last_wrong = false;
    memset(verify_indices, 0, sizeof(verify_indices));

    session_confirm_pending = false;
    session_confirm_return = SCREEN_MAIN_MENU;
    host_unlock_pending = false;
    host_lock_pending = false;
    sign_outcome = SIGN_PENDING;
    sign_expired_pending = false;
    host_passphrase_pending = false;
    memzero(host_passphrase_xfp, sizeof(host_passphrase_xfp));
#if LEEK_HAS_ZCASH
    zsign_review_pending = false;
    zsign_summary = NULL;
    zsign_page = 0;
    addr_check_pending = false;
    memzero(&addr_check, sizeof addr_check);
    zsign_from_sd = false;
    zsign_from_qr = false;
#endif

    lock_timeout_choice = 1;
    lock_timeout_stored_choice = 1;
    brightness_choice = 5;
    zcash_network = 0;
    wrong_network_pending = 0;
    wrong_network_until_us = 0;
    memzero(master_xfp, sizeof(master_xfp));
    memzero(master_zfp, sizeof(master_zfp));
    last_activity_us = 0;
    splash_ok_only = LEEK_HAS_ENCODER;
    pin_idle_since_us = 0;
}

#endif /* LEEK_HOST_TEST */
