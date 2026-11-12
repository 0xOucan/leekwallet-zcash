//! The QR air gap (EXPERIMENTAL, opt-in): the signer view `zw_propose_*`
//! built goes to the board as an animated `ur:leek-pczt`, and the board's
//! `.zsig` comes back as `ur:leek-zsig` through the webview's camera. The
//! `.zsig` then goes to `zw_finish` exactly as one signed over USB or
//! Bluetooth: checked against the view's hash, each signature against its
//! rk, proved, and broadcast after the same checks.
//!
//! The UR is `leek-ur` (host/leek-ur), the implementation zecz uses, tested
//! against the firmware's bc-ur vectors and the shared fixtures in
//! `vectors/ur/`. The webview draws the QR codes and runs the camera; it
//! never encodes or assembles a UR itself.
//!
//! Not behind the `zcash-wallet` feature: encoding a PCZT and reading a
//! `.zsig` need no wallet, and a board-tools build may sign a file this way
//! later.

use std::sync::Mutex;

use leek_ur::{Decoder, Encoder, Received, UrError, UR_PCZT, UR_ZSIG};
use serde::Serialize;
use tauri::State;

/// The fragment sizes the send screen steps through, bytes. 90 is the
/// default (where the Ethereum LeekWallet's benches on this board settled),
/// 50 the Zodl fork's; the larger ones are fewer frames for a camera that
/// resolves the denser code (the board's camera tuning, a 2x/3x zoom, aims
/// at ~150). Module size against frame count: only the bench can say.
///
/// Board bench, 2026-11-10: from a 1080p laptop screen the board found a
/// 90 B code (v8) 253 times and decoded it 3 times; 50 B (v6) at 2x zoom
/// read 58 parts in 70 s. 50 B is the default since; 40 and 30 B (v5) and
/// 25 B (v4, the biggest modules) are for a board at 1x, further away, or
/// reading a phone screen.
pub const FRAGMENTS: [usize; 9] = [25, 30, 40, 50, 90, 120, 150, 200, 250];
/// The frame intervals offered, ms. 500 is the default: a frame stays up at
/// least twice the board camera's capture interval (~7 fps in QVGA today).
pub const INTERVALS: [u64; 7] = [800, 500, 400, 350, 300, 250, 200];

/// Frames asked for at once; the webview asks for the next batch when it
/// runs out, so the fountain keeps going without holding the whole loop.
const MAX_BATCH: u32 = 255;

#[derive(Clone, Debug, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct QrFrames {
    /// Fragments in the message (1 for a still code).
    pub seq_len: u32,
    pub fragment_len: usize,
    /// The longest frame of the loop, in characters: the webview picks one
    /// QR version for it, so the code keeps its size while it animates.
    pub longest: usize,
    /// Frame `start` onwards, uppercase (QR alphanumeric mode).
    pub frames: Vec<String>,
}

/// Frames `start..start+count` of the `ur:leek-pczt` loop of `view`.
pub fn pczt_frames(view: &[u8], max_fragment: usize, start: u64, count: u32) -> Result<QrFrames, String> {
    if !view.starts_with(b"PCZT") {
        return Err("not a PCZT".into());
    }
    let e = Encoder::leek_pczt(view, max_fragment).map_err(|e| e.to_string())?;
    let count = if e.is_single_part() { 1 } else { count.clamp(1, MAX_BATCH) };
    Ok(QrFrames {
        seq_len: e.seq_len(),
        fragment_len: e.fragment_len(),
        longest: e.longest_frame(),
        frames: (start..start + count as u64).map(|i| e.frame(i)).collect(),
    })
}

#[tauri::command]
pub fn qr_pczt_frames(view: Vec<u8>, max_fragment: usize, start: u64, count: u32) -> Result<QrFrames, String> {
    pczt_frames(&view, max_fragment, start, count)
}

/// How to show the loop (`leek_ur::display`, the SDK's `qr_display_hints()`):
/// the same numbers and the same words as Zodl and Zingo get.
#[derive(Clone, Debug, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct DisplayHints {
    /// This build runs on a phone (Android): show `phone_tips` and the
    /// brightness button; the screen is kept on while the code is up
    /// (screen.rs).
    pub phone: bool,
    pub phone_fragment_len: usize,
    pub phone_frame_ms: u32,
    pub large_screen_fragment_len: usize,
    pub large_screen_frame_ms: u32,
    pub quiet_zone_modules: u32,
    /// The QR page's brightness steps (`None`: the system's), never forced.
    pub brightness_levels: Vec<Option<f32>>,
    pub default_brightness: Option<f32>,
    pub phone_tips: Vec<&'static str>,
    pub large_screen_tips: Vec<&'static str>,
    /// By the address QR for the board's Check address (experimental).
    pub address_check_hint: &'static str,
}

pub fn display_hints(phone: bool) -> DisplayHints {
    let h = leek_ur::display_hints();
    DisplayHints {
        phone,
        phone_fragment_len: h.phone_fragment_len,
        phone_frame_ms: h.phone_frame_ms,
        large_screen_fragment_len: h.large_screen_fragment_len,
        large_screen_frame_ms: h.large_screen_frame_ms,
        quiet_zone_modules: h.quiet_zone_modules,
        brightness_levels: h.brightness_levels.to_vec(),
        default_brightness: h.default_brightness,
        phone_tips: h.phone_tips.to_vec(),
        large_screen_tips: h.large_screen_tips.to_vec(),
        address_check_hint: leek_ur::display::ADDRESS_CHECK_HINT,
    }
}

#[tauri::command]
pub fn qr_display_hints() -> DisplayHints {
    display_hints(cfg!(target_os = "android"))
}

/// The board's reply being assembled.
#[derive(Default)]
pub struct QrScanState(Mutex<Decoder>);

#[derive(Clone, Debug, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct ScanProgress {
    /// "accepted", "redundant", "ignored" (not a UR at all, or our own
    /// ur:leek-pczt seen in a mirror), "rejected" (a broken or foreign
    /// part; scanning goes on) or "complete".
    pub state: &'static str,
    pub received: u32,
    /// 0 until the first ur:leek-zsig part.
    pub total: u32,
    /// Why a part was rejected.
    pub error: Option<String>,
    /// The `.zsig`, once complete.
    pub zsig: Option<Vec<u8>>,
}

pub fn feed(d: &mut Decoder, text: &str) -> ScanProgress {
    let text = text.trim();
    let progress = |d: &Decoder, state, error: Option<String>, zsig| ScanProgress {
        state,
        received: d.received(),
        total: d.seq_len(),
        error,
        zsig,
    };
    match leek_ur::part_type(text).as_deref() {
        Some(UR_ZSIG) => {}
        Some(UR_PCZT) | None => return progress(d, "ignored", None, None),
        Some(other) => return progress(d, "rejected", Some(format!("a ur:{other}, not the board's ur:leek-zsig")), None),
    }
    match d.receive(text) {
        Ok(Received::Complete) => match leek_ur::unwrap_bytes(d.message().expect("complete")) {
            Ok(z) => {
                let z = z.to_vec();
                progress(d, "complete", None, Some(z))
            }
            Err(e) => {
                d.reset();
                progress(d, "rejected", Some(e.to_string()), None)
            }
        },
        Ok(Received::Accepted) => progress(d, "accepted", None, None),
        Ok(Received::Redundant) => progress(d, "redundant", None, None),
        Err(e @ UrError::Checksum) => progress(d, "rejected", Some(format!("{e}; starting over")), None),
        Err(e) => progress(d, "rejected", Some(e.to_string()), None),
    }
}

/// Forget any partial reply: the scan screen calls this on entry.
#[tauri::command]
pub fn qr_scan_reset(state: State<'_, QrScanState>) {
    state.0.lock().unwrap_or_else(|p| p.into_inner()).reset();
}

/// One decoded QR text from the camera.
#[tauri::command]
pub fn qr_scan_feed(state: State<'_, QrScanState>, text: String) -> ScanProgress {
    let mut d = state.0.lock().unwrap_or_else(|p| p.into_inner());
    let p = feed(&mut d, &text);
    if p.state == "complete" {
        d.reset();
    }
    p
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn vector(p: &str) -> Vec<u8> {
        std::fs::read(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../vectors").join(p)).unwrap()
    }

    #[test]
    fn a_compact_view_animates_at_the_board_tested_size() {
        let view = vector("pczt/iw_send_nu7_compact.pczt");
        let f = pczt_frames(&view, 50, 0, 40).unwrap();
        assert_eq!((f.seq_len, f.fragment_len), (28, 50));
        assert_eq!(f.frames.len(), 40);
        assert!(f.frames[0].starts_with("UR:LEEK-PCZT/1-28/"));
        assert!(f.frames.iter().all(|x| x.len() <= f.longest));
        // The frames are the shared fixture's, uppercased.
        let fixture = String::from_utf8(vector("ur/iw_send_nu7_compact.50.leek-pczt.txt")).unwrap();
        let parts: Vec<&str> = fixture.lines().filter(|l| !l.starts_with('#') && !l.is_empty()).collect();
        for (a, b) in f.frames.iter().zip(&parts) {
            assert_eq!(a, &b.to_uppercase());
        }
        // A later batch carries on the loop.
        let g = pczt_frames(&view, 50, 40, 10).unwrap();
        assert!(g.frames[0].starts_with("UR:LEEK-PCZT/41-28/"));
        assert!(pczt_frames(b"not a pczt", 50, 0, 1).is_err());
    }

    #[test]
    fn the_board_reply_assembles_from_any_order() {
        let zsig = vector("zsig/iw_send_shielded.zsig");
        let fixture = String::from_utf8(vector("ur/iw_send_shielded.30.leek-zsig.txt")).unwrap();
        let mut parts: Vec<&str> = fixture.lines().filter(|l| !l.starts_with('#') && !l.is_empty()).collect();
        parts.reverse();
        let mut d = Decoder::new();
        assert_eq!(feed(&mut d, "zcash:t1abc").state, "ignored");
        assert_eq!(feed(&mut d, "UR:LEEK-PCZT/AEAEAEAE").state, "ignored");
        assert_eq!(feed(&mut d, "UR:CRYPTO-PSBT/AEAEAEAE").state, "rejected");
        let mut done = None;
        for p in &parts {
            let r = feed(&mut d, &p.to_uppercase());
            assert_ne!(r.state, "rejected", "{r:?}");
            if r.state == "complete" {
                done = r.zsig;
                break;
            }
            assert!(r.total == 4 && r.received < 4, "{r:?}"); // 110 B in 30-byte fragments
        }
        assert_eq!(done.unwrap(), zsig);
        // The still code of the colour panel.
        let one = String::from_utf8(vector("ur/iw_send_shielded.512.leek-zsig.txt")).unwrap();
        let mut d = Decoder::new();
        let line = one.lines().find(|l| l.starts_with("ur:")).unwrap();
        assert_eq!(feed(&mut d, line).zsig.unwrap(), zsig);
    }

    #[test]
    fn the_offered_sizes_fit_the_board() {
        let view = vector("pczt/main_iw_send_nu7_compact.pczt");
        for f in FRAGMENTS {
            let r = pczt_frames(&view, f, 0, 1).unwrap();
            assert!(r.seq_len <= leek_ur::MAX_PARTS && r.fragment_len <= f);
            // airgap.c's 16 KB of fragments.
            assert!(r.seq_len as usize * r.fragment_len <= 16384);
        }
        assert!(INTERVALS.contains(&500) && FRAGMENTS.contains(&50) && FRAGMENTS[0] == 25);
    }

    #[test]
    fn the_display_hints_are_the_sdks_and_offered_here() {
        let h = display_hints(true);
        assert!(h.phone);
        assert_eq!((h.phone_fragment_len, h.phone_frame_ms, h.quiet_zone_modules), (25, 350, 2));
        assert!(FRAGMENTS.contains(&h.phone_fragment_len) && INTERVALS.contains(&(h.phone_frame_ms as u64)));
        assert!(FRAGMENTS.contains(&h.large_screen_fragment_len) && INTERVALS.contains(&(h.large_screen_frame_ms as u64)));
        assert_eq!(h.phone_tips, leek_ur::display::PHONE_TIPS);
        assert!(h.phone_tips[0].contains("anti-flicker"));
        assert!(!display_hints(false).phone);
        let j = serde_json::to_value(display_hints(false)).unwrap();
        assert!(j["phoneTips"].is_array() && j["largeScreenTips"].is_array());
        assert_eq!(j["brightnessLevels"], serde_json::json!([null, 0.6f32, 0.8f32, 1.0]));
        assert!(j["defaultBrightness"].is_null());
        assert!(j["addressCheckHint"].as_str().unwrap().contains("Check address"));
    }
}
