//! **Experimental.** Animated QR codes (Uniform Resources) for the air gap:
//! the board's View key screen (`ur:leek-zcash-accounts`), the signing
//! request a wallet shows the board (`ur:leek-pczt`) and the board's
//! signatures (`ur:leek-zsig`).
//!
//! The parts are byte-identical to bc-ur's (`host/leek-ur`, the same code
//! the companion app and `zecz` use, checked against the firmware's
//! encoder and decoder). A wallet only moves strings: its camera hands each
//! decoded text to [`QrScanner::feed`], and its screen draws
//! [`QrEncoder::frame`] for frame 0, 1, 2, ... in turn.
//!
//! QR is never for pairing. USB and Bluetooth keep the 6-digit code.

use std::sync::{Arc, Mutex, MutexGuard};

use leek_ur::{Decoder, Encoder, Received, UrError, UR_PCZT, UR_ZSIG};
use leek_zcash as lz;

use crate::{AccountEntry, AccountRecord, LeekError, Network, Result};

/// Bytes per fragment the SDK recommends from a computer screen to the
/// board's camera (2x zoom): the board bench of 2026-11-10 read a compact
/// send at 40 B in 60 s and at 50 B in 71 s; 90 B was found but rarely
/// decoded. Phones held close can try [`QR_FRAGMENT_CHOICES`]' larger ones.
pub const QR_FRAGMENT_LEN: u32 = 50;
/// Milliseconds each frame stays up: at least twice the board camera's
/// capture interval.
pub const QR_FRAME_MS: u32 = 500;
/// The sizes a settings screen may offer, bytes per fragment.
pub const QR_FRAGMENT_CHOICES: [u32; 9] = [25, 30, 40, 50, 90, 120, 150, 200, 250];
/// The frame intervals a settings screen may offer, ms.
pub const QR_FRAME_MS_CHOICES: [u32; 7] = [800, 500, 400, 350, 300, 250, 200];
/// Parts of another message in a row after which the scanner follows the
/// new sender instead of refusing it (the board's rule).
pub const QR_SWITCH_AFTER: u32 = 3;
/// Frames [`QrEncoder::frames`] returns at most in one call.
pub const QR_MAX_BATCH: u32 = 255;

/// Recommended settings, for a settings screen and its defaults.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct QrDefaults {
    /// Bytes per fragment, host screen to the board's camera.
    pub fragment_len: u32,
    /// Milliseconds per frame.
    pub frame_ms: u32,
    pub fragment_choices: Vec<u32>,
    pub frame_ms_choices: Vec<u32>,
    /// The board's camera zoom the defaults were measured at.
    pub board_zoom: u32,
}

#[uniffi::export]
pub fn qr_defaults() -> QrDefaults {
    QrDefaults {
        fragment_len: QR_FRAGMENT_LEN,
        frame_ms: QR_FRAME_MS,
        fragment_choices: QR_FRAGMENT_CHOICES.to_vec(),
        frame_ms_choices: QR_FRAME_MS_CHOICES.to_vec(),
        board_zoom: 2,
    }
}

/// How to show the animated `ur:leek-pczt` so the board's camera reads it
/// (experimental). Brightness is the user's (system by default, adjustable
/// on the QR page); the screen is kept on. From the bench of 2026-11-11 (docs/zcash/QR-BENCH.md,
/// "Signing with a phone"), with the board firmware of that day; older
/// firmware reads [`qr_defaults`] better. The same values and words as the
/// companion app (`leek_ur::display`).
#[derive(Clone, Debug, PartialEq, uniffi::Record)]
pub struct QrDisplayHints {
    /// Bytes per fragment from a phone screen: 25 (QR v4, the biggest modules).
    pub phone_fragment_len: u32,
    /// Milliseconds per frame from a phone: 350 (300–350 read).
    pub phone_frame_ms: u32,
    /// The board's zoom for a phone: 3x.
    pub phone_board_zoom: u32,
    /// The phone's distance from the board's fixed-focus camera, cm: 30.
    pub phone_distance_cm: u32,
    /// Bytes per fragment from a laptop or tablet screen: 50 (39–88 read).
    pub large_screen_fragment_len: u32,
    /// Milliseconds per frame from a laptop or tablet: 300 (250–350 read).
    pub large_screen_frame_ms: u32,
    /// The board's zoom for a laptop or tablet: 2x.
    pub large_screen_board_zoom: u32,
    /// Deprecated, always false: do not force full brightness. On an OLED
    /// phone 100 % washed the code out for the board's camera (2026-11-11).
    /// Offer [`QrDisplayHints::brightness_levels`] instead.
    pub max_brightness: bool,
    /// The brightness steps to offer on the QR page, as a window brightness
    /// 0.0–1.0; `None` is the system's own (no override): [system, 0.6, 0.8,
    /// 1.0]. A single button cycling them is enough. Android
    /// `WindowManager.LayoutParams.screenBrightness` (`None` =
    /// `BRIGHTNESS_OVERRIDE_NONE`), iOS `UIScreen.brightness` (`None` = leave
    /// it). Remember the user's choice; restore the system's when the code goes.
    pub brightness_levels: Vec<Option<f32>>,
    /// The step the QR page opens at the first time: `None`, the system's.
    pub default_brightness: Option<f32>,
    /// Keep the screen on while the code shows (Android
    /// `FLAG_KEEP_SCREEN_ON`, iOS `isIdleTimerDisabled`), and restore it after.
    pub keep_screen_on: bool,
    /// The page behind the code, full screen: "#000000".
    pub page_background: String,
    /// White modules round the code on that page: 2.
    pub quiet_zone_modules: u32,
    /// QR error correction: "L".
    pub error_correction: String,
    /// Every frame is uppercase: encode it in QR alphanumeric mode.
    pub uppercase_alphanumeric: bool,
    /// Whole device pixels per module, no smoothing.
    pub whole_pixel_modules: bool,
    /// Short lines to show by the code on a phone (plain English). The
    /// first, anti-flicker on OLED phones, is one no app can switch itself.
    pub phone_tips: Vec<String>,
    /// Short lines to show by the code on a laptop or tablet.
    pub large_screen_tips: Vec<String>,
}

#[uniffi::export]
pub fn qr_display_hints() -> QrDisplayHints {
    let h = leek_ur::display_hints();
    let strings = |v: &[&str]| v.iter().map(|s| s.to_string()).collect();
    QrDisplayHints {
        phone_fragment_len: h.phone_fragment_len as u32,
        phone_frame_ms: h.phone_frame_ms,
        phone_board_zoom: h.phone_board_zoom,
        phone_distance_cm: h.phone_distance_cm,
        large_screen_fragment_len: h.large_screen_fragment_len as u32,
        large_screen_frame_ms: h.large_screen_frame_ms,
        large_screen_board_zoom: h.large_screen_board_zoom,
        max_brightness: h.max_brightness,
        brightness_levels: h.brightness_levels.to_vec(),
        default_brightness: h.default_brightness,
        keep_screen_on: h.keep_screen_on,
        page_background: h.page_background.into(),
        quiet_zone_modules: h.quiet_zone_modules,
        error_correction: h.error_correction.into(),
        uppercase_alphanumeric: h.uppercase_alphanumeric,
        whole_pixel_modules: h.whole_pixel_modules,
        phone_tips: strings(h.phone_tips),
        large_screen_tips: strings(h.large_screen_tips),
    }
}

/// **Experimental.** The text of the QR to show for the board's Check
/// address (Main menu → Check address): the board scans it and says whether
/// the address is its own, with the account and diversifier index. `address`
/// is a unified, Sapling or transparent address or a `zcash:` URI with one
/// address; the result is the plain address (never uppercased: Base58 keeps
/// its case). A wallet's usual Receive QR of the plain address works too.
/// Not an address: `Parse` (512), as `to_checkable_ua`. Over USB or Bluetooth use
/// `LeekDevice.show_address` instead.
#[uniffi::export]
pub fn address_check_qr(address: String) -> Result<String> {
    Ok(lz::address_check_payload(&address)?)
}

/// **Experimental.** The line to show by that QR: "On the board: Main menu →
/// Check address (experimental), then scan this code."
#[uniffi::export]
pub fn address_check_hint() -> String {
    leek_ur::display::ADDRESS_CHECK_HINT.into()
}

/// The UR type a scanned string names (lowercase), or None for a text that
/// is not a UR (a `zcash:` URI, an address).
#[uniffi::export]
pub fn qr_part_type(text: String) -> Option<String> {
    leek_ur::part_type(&text)
}

fn ur_err(e: UrError) -> LeekError {
    match e {
        UrError::TooLarge => lz::LeekError::TooLarge,
        UrError::Empty => lz::LeekError::MissingData,
        _ => lz::LeekError::Parse,
    }
    .into()
}

// -------------------------------------------------------------- the encoder

/// The frames of one animated QR. Every frame is uppercase (the QR's
/// alphanumeric mode) and at most [`QrEncoder::longest_frame`] characters,
/// so the code keeps one size while it animates: pick the QR version once.
#[derive(uniffi::Object)]
pub struct QrEncoder {
    inner: Encoder,
}

#[uniffi::export]
impl QrEncoder {
    /// `ur:leek-pczt` for the board: `view` is `signer_view_bytes` (after
    /// `prepare_for_signer`). `max_fragment_len` is clamped to 10..=512;
    /// [`QR_FRAGMENT_LEN`] is the default.
    #[uniffi::constructor]
    pub fn pczt(view: Vec<u8>, max_fragment_len: u32) -> Result<Arc<Self>> {
        if !view.starts_with(b"PCZT") {
            return Err(lz::LeekError::Parse.into());
        }
        let inner = Encoder::leek_pczt(&view, max_fragment_len as usize).map_err(ur_err)?;
        Ok(Arc::new(QrEncoder { inner }))
    }

    /// `ur:leek-zcash-accounts`: what the board's View key screen shows,
    /// for a wallet passing an account on, and for tests.
    #[uniffi::constructor]
    pub fn account_record(record: AccountRecord, max_fragment_len: u32) -> Result<Arc<Self>> {
        let body = crate::encode_account_record(record)?;
        let inner =
            Encoder::new(lz::ACCOUNT_RECORD_UR_TYPE, &body, max_fragment_len as usize).map_err(ur_err)?;
        Ok(Arc::new(QrEncoder { inner }))
    }

    /// `ur:leek-zsig`: the board's reply, for tests and simulators.
    #[uniffi::constructor]
    pub fn zsig(zsig: Vec<u8>, max_fragment_len: u32) -> Result<Arc<Self>> {
        let inner = Encoder::leek_zsig(&zsig, max_fragment_len as usize).map_err(ur_err)?;
        Ok(Arc::new(QrEncoder { inner }))
    }

    pub fn ur_type(&self) -> String {
        self.inner.ur_type().into()
    }

    /// Fragments in the message: about how many frames a good camera needs.
    pub fn seq_len(&self) -> u32 {
        self.inner.seq_len()
    }

    pub fn fragment_len(&self) -> u32 {
        self.inner.fragment_len() as u32
    }

    /// One frame holds it all: show it still.
    pub fn is_single_part(&self) -> bool {
        self.inner.is_single_part()
    }

    /// The longest frame, in characters: choose the QR version for this.
    pub fn longest_frame(&self) -> u32 {
        self.inner.longest_frame() as u32
    }

    /// Frame `index` (0-based) of an endless loop: the first `seq_len`
    /// are the fragments themselves, the rest fountain mixes that fill in
    /// whatever the camera missed. Keep counting up; it wraps by itself.
    pub fn frame(&self, index: u64) -> String {
        self.inner.frame(index)
    }

    /// `count` frames from `start` (at most [`QR_MAX_BATCH`]; one for a
    /// single-part message).
    pub fn frames(&self, start: u64, count: u32) -> Vec<String> {
        let count = if self.inner.is_single_part() { 1 } else { count.clamp(1, QR_MAX_BATCH) };
        (start..start + count as u64).map(|i| self.inner.frame(i)).collect()
    }
}

// -------------------------------------------------------------- the scanner

/// What one scanned text did.
#[derive(Clone, Copy, Debug, PartialEq, Eq, uniffi::Enum)]
pub enum QrPartState {
    /// A new part; keep scanning.
    Accepted,
    /// A valid part with nothing new (a repeat).
    Redundant,
    /// Not for this scanner and harmless: not a UR, or one of the codes
    /// the wallet itself shows (seen in a mirror or on a second screen).
    Ignored,
    /// A broken part or another UR type; nothing changed, keep scanning.
    Rejected,
    /// The message assembled but cannot be used (another network, a
    /// malformed record). The scanner started over; stop the camera and
    /// show `error`.
    Refused,
    /// Done: read the result with [`QrScanner::account_record`] or
    /// [`QrScanner::zsig`].
    Complete,
}

/// The scanner's state after a text, for a progress bar.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct QrProgress {
    pub state: QrPartState,
    /// Fragments known.
    pub received: u32,
    /// Fragments in the message; 0 until the first part of it.
    pub total: u32,
    /// 0..=100.
    pub percent: u8,
    /// Why a part was rejected or the message refused. Never contains key
    /// material: safe to show and log.
    pub error: Option<String>,
}

/// What a [`QrScanner`] waits for.
#[derive(Clone, Copy, Debug, PartialEq, Eq, uniffi::Enum)]
pub enum QrScanKind {
    /// `ur:leek-zcash-accounts` from the board's View key screen.
    AccountRecord,
    /// `ur:leek-zsig`, the board's signatures after a `ur:leek-pczt`.
    Zsig,
}

enum Done {
    Record(AccountRecord),
    Zsig(Vec<u8>),
}

struct Scan {
    decoder: Decoder,
    /// Parts of another message in a row.
    others: u32,
    done: Option<Done>,
}

/// Assembles one message from camera texts. Feed every decoded string,
/// in any order and with repeats; it is safe to call from the camera's
/// thread.
#[derive(uniffi::Object)]
pub struct QrScanner {
    kind: QrScanKind,
    network: Option<Network>,
    scan: Mutex<Scan>,
}

impl QrScanner {
    fn lock(&self) -> MutexGuard<'_, Scan> {
        self.scan.lock().unwrap_or_else(|p| p.into_inner())
    }

    fn wanted(&self) -> &'static str {
        match self.kind {
            QrScanKind::AccountRecord => lz::ACCOUNT_RECORD_UR_TYPE,
            QrScanKind::Zsig => UR_ZSIG,
        }
    }

    /// Types this scanner's own wallet shows, so a reflection is ignored.
    fn ours(&self, t: &str) -> bool {
        t == UR_PCZT || (self.kind == QrScanKind::AccountRecord && t == UR_ZSIG)
    }

    fn finish(&self, message: &[u8]) -> std::result::Result<Done, String> {
        match self.kind {
            QrScanKind::Zsig => {
                let z = leek_ur::unwrap_bytes(message).map_err(|e| e.to_string())?;
                if z.is_empty() {
                    return Err("an empty reply".into());
                }
                Ok(Done::Zsig(z.to_vec()))
            }
            QrScanKind::AccountRecord => {
                let r = lz::AccountRecord::decode(message)
                    .map_err(|e| format!("not a readable account record: {e}"))?;
                if r.accounts.is_empty() {
                    return Err("the account record holds no account".into());
                }
                if let Some(want) = self.network {
                    for a in &r.accounts {
                        let theirs: Network =
                            a.network().map_err(|e| format!("the viewing key: {e}"))?.into();
                        if theirs != want {
                            return Err(format!(
                                "this is a {} viewing key, and this wallet is on {}",
                                label(theirs),
                                label(want)
                            ));
                        }
                    }
                }
                Ok(Done::Record(AccountRecord {
                    seed_fingerprint: r.seed_fingerprint.to_vec(),
                    accounts: r
                        .accounts
                        .into_iter()
                        .map(|a| AccountEntry { ufvk: a.ufvk, index: a.index, name: a.name, birthday: a.birthday })
                        .collect(),
                }))
            }
        }
    }
}

fn label(n: Network) -> &'static str {
    match n {
        Network::Main => "Mainnet",
        Network::Test => "Testnet",
    }
}

fn progress(d: &Decoder, state: QrPartState, error: Option<String>) -> QrProgress {
    let (received, total) = (d.received(), d.seq_len());
    let percent = if state == QrPartState::Complete {
        100
    } else {
        (received * 100).checked_div(total).map_or(0, |p| p.min(99) as u8)
    };
    QrProgress { state, received, total, percent, error }
}

#[uniffi::export]
impl QrScanner {
    /// A scanner for `kind`. For [`QrScanKind::AccountRecord`], `network`
    /// (when given) refuses a viewing key of the other network.
    #[uniffi::constructor]
    pub fn new(kind: QrScanKind, network: Option<Network>) -> Arc<Self> {
        Arc::new(QrScanner {
            kind,
            network,
            scan: Mutex::new(Scan { decoder: Decoder::new(), others: 0, done: None }),
        })
    }

    /// One decoded QR text.
    pub fn feed(&self, text: String) -> QrProgress {
        let text = text.trim();
        let mut s = self.lock();
        if s.done.is_some() {
            return progress(&s.decoder, QrPartState::Complete, None);
        }
        match leek_ur::part_type(text) {
            None => return progress(&s.decoder, QrPartState::Ignored, None),
            Some(t) if self.ours(&t) => return progress(&s.decoder, QrPartState::Ignored, None),
            Some(t) if t != self.wanted() => {
                let why = format!("a ur:{t}, not ur:{}", self.wanted());
                return progress(&s.decoder, QrPartState::Rejected, Some(why));
            }
            Some(_) => {}
        }
        let mut got = s.decoder.receive(text);
        if got == Err(UrError::Mismatch) {
            // The sender moved on to another message (a new request, a
            // board shown again): follow it after a few parts in a row.
            s.others += 1;
            if s.others >= QR_SWITCH_AFTER {
                s.decoder.reset();
                s.others = 0;
                got = s.decoder.receive(text);
            }
        } else if got.is_ok() {
            s.others = 0;
        }
        match got {
            Ok(Received::Complete) => {
                let message = s.decoder.message().expect("complete").to_vec();
                match self.finish(&message) {
                    Ok(done) => {
                        s.done = Some(done);
                        progress(&s.decoder, QrPartState::Complete, None)
                    }
                    Err(why) => {
                        s.decoder.reset();
                        progress(&s.decoder, QrPartState::Refused, Some(why))
                    }
                }
            }
            Ok(Received::Accepted) => progress(&s.decoder, QrPartState::Accepted, None),
            Ok(Received::Redundant) => progress(&s.decoder, QrPartState::Redundant, None),
            Err(e @ UrError::Checksum) => {
                progress(&s.decoder, QrPartState::Rejected, Some(format!("{e}; starting over")))
            }
            Err(e) => progress(&s.decoder, QrPartState::Rejected, Some(e.to_string())),
        }
    }

    /// Start over (a new scan, or after [`QrPartState::Refused`]).
    pub fn reset(&self) {
        let mut s = self.lock();
        s.decoder.reset();
        s.others = 0;
        s.done = None;
    }

    pub fn is_complete(&self) -> bool {
        self.lock().done.is_some()
    }

    /// The record, once complete (an [`QrScanKind::AccountRecord`] scanner).
    /// Check the seed fingerprint and import each UFVK as a hardware
    /// (board) account; derive the address from the UFVK yourself.
    pub fn account_record(&self) -> Option<AccountRecord> {
        match &self.lock().done {
            Some(Done::Record(r)) => Some(r.clone()),
            _ => None,
        }
    }

    /// The `.zsig`, once complete (a [`QrScanKind::Zsig`] scanner): pass it
    /// to `apply_zsig` with the view that was shown.
    pub fn zsig(&self) -> Option<Vec<u8>> {
        match &self.lock().done {
            Some(Done::Zsig(z)) => Some(z.clone()),
            _ => None,
        }
    }
}
