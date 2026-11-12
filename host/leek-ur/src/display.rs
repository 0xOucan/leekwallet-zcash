//! How to show an animated `ur:leek-pczt` so the board's camera reads it
//! (QR signing). One set of numbers and words for every
//! wallet: the companion app, `leek-zcash-ffi`'s `qr_display_hints()` (Zodl,
//! Zingo) and the docs (`docs/zcash/FORMATS.md`, "Animated QR").
//!
//! Measured on the colour kit on 2026-11-11, with the board firmware's
//! decoder fixes of that day (`docs/zcash/QR-BENCH.md`, "Signing with a
//! phone"):
//!
//! - A phone read best with big codes (25 B fragments, QR v4) at 300–350 ms,
//!   full screen on a black page with a 2-module white quiet zone, the board
//!   at 3x zoom about 30 cm away (its lens is fixed-focus).
//! - Brightness is the user's: forcing the window to 100 % was the blocker
//!   on an OLED phone (the board found the code but read it washed out).
//!   So a wallet keeps the system brightness by default and offers
//!   [`BRIGHTNESS_LEVELS`] on the QR page; the board firmware adjusts its
//!   exposure both ways. The screen is still kept on.
//! - On an OLED phone, the system's anti-flicker (DC dimming) setting helped
//!   a lot: PWM dimming makes the board's rolling-shutter camera see bands
//!   across the code. No app can switch it, so the wallet tells the user.
//! - LCD screens (laptops, tablets) read at 2x with 39–88 B fragments at
//!   250–350 ms.
//!
//! The code itself: error correction L, uppercase in alphanumeric mode, one
//! QR version for the whole loop (`Encoder::longest_frame`), black modules
//! on white, a whole number of device pixels per module.

/// Bytes per fragment from a phone screen (QR v4, the biggest modules).
pub const PHONE_FRAGMENT_LEN: usize = 25;
/// Milliseconds per frame from a phone (300–350 ms read; the slower end).
pub const PHONE_FRAME_MS: u32 = 350;
/// The board's zoom for a phone held about [`PHONE_DISTANCE_CM`] away.
pub const PHONE_BOARD_ZOOM: u32 = 3;
/// Where the board's fixed-focus lens is sharp.
pub const PHONE_DISTANCE_CM: u32 = 30;

/// Bytes per fragment from a laptop or tablet screen (39–88 B read).
pub const LARGE_SCREEN_FRAGMENT_LEN: usize = 50;
/// Milliseconds per frame from a laptop or tablet (250–350 ms read).
pub const LARGE_SCREEN_FRAME_MS: u32 = 300;
/// The board's zoom for a laptop or tablet (the Scan screen opens at it).
pub const LARGE_SCREEN_BOARD_ZOOM: u32 = 2;

/// White modules round the code, on a black page (full screen).
pub const QUIET_ZONE_MODULES: u32 = 2;
/// The page behind the code and its quiet zone.
pub const PAGE_BACKGROUND: &str = "#000000";
/// QR error correction level: L (the most data per module).
pub const ERROR_CORRECTION: &str = "L";

/// The brightness steps a QR page offers, as a window brightness 0.0–1.0;
/// `None` is the system's own (no override), the first and the default.
/// Never force 100 %: on an OLED phone it washed the code out for the board.
pub const BRIGHTNESS_LEVELS: [Option<f32>; 4] = [None, Some(0.6), Some(0.8), Some(1.0)];
/// The brightness a QR page opens at: the system's (no override).
pub const DEFAULT_BRIGHTNESS: Option<f32> = None;

/// What to tell the user before showing the code from a phone.
pub const PHONE_TIPS: [&str; 4] = [
    "OLED phone: turn on anti-flicker (DC dimming) in the display settings.",
    "If the board finds the code but does not read it, change the brightness here: lower first if the code looks washed out on the board, higher if it looks dark.",
    "Big codes, full screen; the screen stays on.",
    "Board at 3x, the phone about 30 cm from its camera.",
];

/// What to tell the user before showing the code from a laptop or tablet.
pub const LARGE_SCREEN_TIPS: [&str; 2] = [
    "Screen brightness up.",
    "Board at 2x (the Scan screen opens at 2x).",
];

/// What to show by an address QR for the board's Check address: the board reads a plain address or `zcash:` URI and says
/// whether it is its own, with the account and diversifier index.
pub const ADDRESS_CHECK_HINT: &str =
    "On the board: Main menu → Check address, then scan this code.";

/// The display rules as one value.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DisplayHints {
    pub phone_fragment_len: usize,
    pub phone_frame_ms: u32,
    pub phone_board_zoom: u32,
    pub phone_distance_cm: u32,
    pub large_screen_fragment_len: usize,
    pub large_screen_frame_ms: u32,
    pub large_screen_board_zoom: u32,
    /// Deprecated, always false: never force full brightness (see
    /// `brightness_levels`). Kept so older callers stop forcing it.
    pub max_brightness: bool,
    /// The brightness steps to offer on the QR page (`None` = system).
    pub brightness_levels: &'static [Option<f32>],
    /// The step the QR page opens at: `None`, the system's brightness.
    pub default_brightness: Option<f32>,
    /// Keep the screen on while the code shows (restore after).
    pub keep_screen_on: bool,
    pub page_background: &'static str,
    pub quiet_zone_modules: u32,
    pub error_correction: &'static str,
    /// Every frame is uppercase: encode it in QR alphanumeric mode.
    pub uppercase_alphanumeric: bool,
    /// Draw each module as a whole number of device pixels (no smoothing).
    pub whole_pixel_modules: bool,
    pub phone_tips: &'static [&'static str],
    pub large_screen_tips: &'static [&'static str],
}

/// The display rules (see the module documentation).
pub const fn display_hints() -> DisplayHints {
    DisplayHints {
        phone_fragment_len: PHONE_FRAGMENT_LEN,
        phone_frame_ms: PHONE_FRAME_MS,
        phone_board_zoom: PHONE_BOARD_ZOOM,
        phone_distance_cm: PHONE_DISTANCE_CM,
        large_screen_fragment_len: LARGE_SCREEN_FRAGMENT_LEN,
        large_screen_frame_ms: LARGE_SCREEN_FRAME_MS,
        large_screen_board_zoom: LARGE_SCREEN_BOARD_ZOOM,
        max_brightness: false,
        brightness_levels: &BRIGHTNESS_LEVELS,
        default_brightness: DEFAULT_BRIGHTNESS,
        keep_screen_on: true,
        page_background: PAGE_BACKGROUND,
        quiet_zone_modules: QUIET_ZONE_MODULES,
        error_correction: ERROR_CORRECTION,
        uppercase_alphanumeric: true,
        whole_pixel_modules: true,
        phone_tips: &PHONE_TIPS,
        large_screen_tips: &LARGE_SCREEN_TIPS,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{Encoder, MAX_FRAGMENT_LEN, MIN_FRAGMENT_LEN};

    #[test]
    fn the_hints_are_the_bench_results() {
        let h = display_hints();
        assert_eq!((h.phone_fragment_len, h.phone_frame_ms, h.phone_board_zoom), (25, 350, 3));
        assert_eq!((h.large_screen_fragment_len, h.large_screen_frame_ms, h.large_screen_board_zoom), (50, 300, 2));
        assert!(!h.max_brightness && h.keep_screen_on && h.uppercase_alphanumeric);
        assert_eq!(h.brightness_levels, &[None, Some(0.6), Some(0.8), Some(1.0)]);
        assert_eq!(h.default_brightness, None);
        assert_eq!((h.page_background, h.quiet_zone_modules, h.error_correction), ("#000000", 2, "L"));
        assert!(h.phone_tips.iter().any(|t| t.contains("anti-flicker") && t.contains("DC dimming")));
        assert!(!h.phone_tips.iter().any(|t| t.contains("100 %")));
        assert!(h.phone_tips.iter().any(|t| t.contains("brightness") && t.contains("lower first") && t.contains("washed out")));
        assert!(h.phone_tips.iter().any(|t| t.contains("screen stays on")));
        assert!(h.phone_tips.iter().any(|t| t.contains("3x") && t.contains("30 cm")));
        assert!(h.large_screen_tips.iter().any(|t| t.contains("2x")));
        assert!(ADDRESS_CHECK_HINT.contains("Main menu → Check address, then scan") && !ADDRESS_CHECK_HINT.contains("experimental"));
        for f in [h.phone_fragment_len, h.large_screen_fragment_len] {
            assert!((MIN_FRAGMENT_LEN..=MAX_FRAGMENT_LEN).contains(&f));
        }
    }

    #[test]
    fn the_frames_are_uppercase_alphanumeric() {
        let e = Encoder::leek_pczt(&[0x50; 600], PHONE_FRAGMENT_LEN).unwrap();
        const ALNUM: &str = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";
        for i in 0..e.seq_len() as u64 + 3 {
            assert!(e.frame(i).chars().all(|c| ALNUM.contains(c)));
        }
    }
}
