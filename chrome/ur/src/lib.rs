//! `host/leek-ur` for the Chrome extension's pages (Sign with QR, and the
//! account import from the board's View key).
//!
//! The extension never encodes or assembles a UR in JavaScript: the frames
//! of the `ur:leek-pczt` loop and the assembly of the board's
//! `ur:leek-zsig` (or `ur:leek-zcash-accounts`) are this crate, which is
//! leek-ur itself (a path dependency, the code zecz, the desktop app,
//! Android and the SDK run), behind a few wasm-bindgen classes. The page
//! draws the codes and runs the camera.
//!
//! Why apart from the engine (chrome/engine): the engine's worker runs one
//! call at a time and a sync holds it for minutes, while a QR loop needs a
//! frame every few hundred milliseconds and the camera a decoder per frame.
//! This module is small (no threads, no C), loads in the page itself and
//! answers at once. The `.zsig` it assembles still goes to the engine's
//! `finish`, which checks it as one signed over USB.
//!
//! [`Scan::feed`] is `app/src-tauri/src/airgap.rs` `feed` (and
//! `qr_accounts.rs` for the account record): the same states, the same
//! rules for codes that are not the one awaited.

use leek_ur::{Decoder, Encoder, Received, UrError, UR_PCZT, UR_ZSIG};
use wasm_bindgen::prelude::*;

/// The board's View key record (`leek_zcash::ACCOUNT_RECORD_UR_TYPE`).
pub const UR_ACCOUNTS: &str = "leek-zcash-accounts";

fn js_err(e: UrError) -> JsError {
    JsError::new(&e.to_string())
}

/// A looping multi-part UR: `frame(i)` for i = 0, 1, 2, … (the fountain
/// keeps going past `seqLen`), uppercase for QR alphanumeric mode.
#[wasm_bindgen]
pub struct UrEncoder {
    e: Encoder,
}

#[wasm_bindgen]
impl UrEncoder {
    /// The signer view (`*.pczt` bytes) as `ur:leek-pczt`.
    #[wasm_bindgen(js_name = leekPczt)]
    pub fn leek_pczt(view: &[u8], max_fragment: usize) -> Result<UrEncoder, JsError> {
        if !view.starts_with(b"PCZT") {
            return Err(JsError::new("not a PCZT"));
        }
        Ok(UrEncoder { e: Encoder::leek_pczt(view, max_fragment).map_err(js_err)? })
    }

    /// A `.zsig` as `ur:leek-zsig`, as the board shows it (tests).
    #[wasm_bindgen(js_name = leekZsig)]
    pub fn leek_zsig(zsig: &[u8], max_fragment: usize) -> Result<UrEncoder, JsError> {
        Ok(UrEncoder { e: Encoder::leek_zsig(zsig, max_fragment).map_err(js_err)? })
    }

    /// Any type, `message` as is (the account record is not wrapped).
    #[wasm_bindgen(constructor)]
    pub fn new(ur_type: &str, message: &[u8], max_fragment: usize) -> Result<UrEncoder, JsError> {
        Ok(UrEncoder { e: Encoder::new(ur_type, message, max_fragment).map_err(js_err)? })
    }

    #[wasm_bindgen(getter, js_name = seqLen)]
    pub fn seq_len(&self) -> u32 {
        self.e.seq_len()
    }

    #[wasm_bindgen(getter, js_name = fragmentLen)]
    pub fn fragment_len(&self) -> usize {
        self.e.fragment_len()
    }

    /// The longest frame of the loop, in characters: one QR version for it
    /// keeps the code the same size while it animates.
    #[wasm_bindgen(getter)]
    pub fn longest(&self) -> usize {
        self.e.longest_frame()
    }

    #[wasm_bindgen(getter, js_name = singlePart)]
    pub fn single_part(&self) -> bool {
        self.e.is_single_part()
    }

    /// Frame `index` of the loop, uppercase.
    pub fn frame(&self, index: u32) -> String {
        self.e.frame(u64::from(index))
    }
}

#[derive(Clone, Copy, PartialEq)]
enum Want {
    Zsig,
    Accounts,
    /// Our own loop, read back: the tests' stand-in board, and a check
    /// that the drawn codes carry the signer view.
    Pczt,
}

/// The board's reply being assembled from camera reads.
#[wasm_bindgen]
pub struct UrScan {
    d: Decoder,
    want: Want,
    error: Option<String>,
    message: Option<Vec<u8>>,
}

#[wasm_bindgen]
impl UrScan {
    /// `want`: "leek-zsig" (Sign with QR's reply), "leek-zcash-accounts"
    /// (the board's View key), or "leek-pczt" (reading our own loop back,
    /// for tests).
    #[wasm_bindgen(constructor)]
    pub fn new(want: &str) -> Result<UrScan, JsError> {
        let want = match want {
            UR_ZSIG => Want::Zsig,
            UR_ACCOUNTS => Want::Accounts,
            UR_PCZT => Want::Pczt,
            other => return Err(JsError::new(&format!("cannot scan for ur:{other}"))),
        };
        Ok(UrScan { d: Decoder::new(), want, error: None, message: None })
    }

    fn want_type(&self) -> &'static str {
        match self.want {
            Want::Zsig => UR_ZSIG,
            Want::Accounts => UR_ACCOUNTS,
            Want::Pczt => UR_PCZT,
        }
    }

    /// One decoded QR text. Returns "accepted", "redundant", "ignored" (not
    /// a UR, or one of our own types: the page's ur:leek-pczt in a mirror),
    /// "rejected" (a broken or foreign part; scanning goes on, `error` says
    /// why) or "complete" (`takeMessage()` has it; the decoder starts over).
    pub fn feed(&mut self, text: &str) -> String {
        self.error = None;
        let text = text.trim();
        let want = self.want_type();
        match leek_ur::part_type(text) {
            Some(t) if t == want => {}
            None => return "ignored".into(),
            Some(t) if t == UR_PCZT => return "ignored".into(),
            Some(t) if self.want == Want::Accounts && t == UR_ZSIG => return "ignored".into(),
            Some(other) => {
                self.error = Some(format!("a ur:{other}, not the board's ur:{want}"));
                return "rejected".into();
            }
        }
        match self.d.receive(text) {
            Ok(Received::Complete) => {
                let msg = self.d.message().expect("complete").to_vec();
                self.d.reset();
                let msg = match self.want {
                    // The .zsig is a CBOR byte string; the record is as is.
                    Want::Zsig | Want::Pczt => match leek_ur::unwrap_bytes(&msg) {
                        Ok(z) => z.to_vec(),
                        Err(e) => {
                            self.error = Some(e.to_string());
                            return "rejected".into();
                        }
                    },
                    Want::Accounts => msg,
                };
                self.message = Some(msg);
                "complete".into()
            }
            Ok(Received::Accepted) => "accepted".into(),
            Ok(Received::Redundant) => "redundant".into(),
            Err(e @ UrError::Checksum) => {
                self.error = Some(format!("{e}; starting over"));
                "rejected".into()
            }
            Err(e) => {
                self.error = Some(e.to_string());
                "rejected".into()
            }
        }
    }

    /// Fragments known so far (0 right after "complete": it started over).
    #[wasm_bindgen(getter)]
    pub fn received(&self) -> u32 {
        self.d.received()
    }

    /// Fragments in the message, 0 until the first part of the awaited type.
    #[wasm_bindgen(getter)]
    pub fn total(&self) -> u32 {
        self.d.seq_len()
    }

    /// Why the last part was rejected.
    #[wasm_bindgen(getter)]
    pub fn error(&self) -> Option<String> {
        self.error.clone()
    }

    /// The assembled message once "complete" (the `.zsig`, or the record's
    /// CBOR), handed over once.
    #[wasm_bindgen(js_name = takeMessage)]
    pub fn take_message(&mut self) -> Option<Vec<u8>> {
        self.message.take()
    }

    /// Forget any partial reply.
    pub fn reset(&mut self) {
        self.d.reset();
        self.error = None;
        self.message = None;
    }
}

/// How to show the loop from a laptop or desktop screen
/// (`leek_ur::display`): fragment bytes and frame milliseconds measured on
/// the board, the white quiet zone round the code on its black page, and
/// the words to show by it. No brightness steps: a desktop page cannot set
/// the screen's.
#[wasm_bindgen(js_name = displayHints)]
pub fn display_hints() -> DisplayHints {
    let h = leek_ur::display_hints();
    DisplayHints {
        fragment_len: h.large_screen_fragment_len,
        frame_ms: h.large_screen_frame_ms,
        board_zoom: h.large_screen_board_zoom,
        quiet_zone_modules: h.quiet_zone_modules,
    }
}

#[wasm_bindgen]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DisplayHints {
    #[wasm_bindgen(js_name = fragmentLen)]
    pub fragment_len: usize,
    #[wasm_bindgen(js_name = frameMs)]
    pub frame_ms: u32,
    #[wasm_bindgen(js_name = boardZoom)]
    pub board_zoom: u32,
    #[wasm_bindgen(js_name = quietZoneModules)]
    pub quiet_zone_modules: u32,
}

/// The tips for a laptop or tablet screen, in order.
#[wasm_bindgen(js_name = largeScreenTips)]
pub fn large_screen_tips() -> Vec<String> {
    leek_ur::display::LARGE_SCREEN_TIPS.iter().map(|s| s.to_string()).collect()
}

/// The page behind the code: black.
#[wasm_bindgen(js_name = pageBackground)]
pub fn page_background() -> String {
    leek_ur::display::PAGE_BACKGROUND.into()
}

/// The largest fragment the board takes, and the most parts.
#[wasm_bindgen(js_name = maxFragmentLen)]
pub fn max_fragment_len() -> usize {
    leek_ur::MAX_FRAGMENT_LEN
}

#[cfg(test)]
mod tests {
    use super::*;

    fn vector(p: &str) -> Vec<u8> {
        std::fs::read(concat!(env!("CARGO_MANIFEST_DIR"), "/../../vectors/").to_string() + p).unwrap()
    }

    #[test]
    fn the_loop_is_the_shared_fixture() {
        let view = vector("pczt/iw_send_nu7_compact.pczt");
        let e = UrEncoder { e: Encoder::leek_pczt(&view, 50).unwrap() };
        assert_eq!((e.seq_len(), e.fragment_len()), (28, 50));
        let fixture = String::from_utf8(vector("ur/iw_send_nu7_compact.50.leek-pczt.txt")).unwrap();
        let parts: Vec<&str> = fixture.lines().filter(|l| !l.starts_with('#') && !l.is_empty()).collect();
        for (i, p) in parts.iter().enumerate() {
            assert_eq!(e.frame(i as u32), p.to_uppercase());
        }
    }

    #[test]
    fn the_reply_assembles_and_others_are_told_apart() {
        let zsig = vector("zsig/iw_send_shielded.zsig");
        let fixture = String::from_utf8(vector("ur/iw_send_shielded.30.leek-zsig.txt")).unwrap();
        let mut s = UrScan { d: Decoder::new(), want: Want::Zsig, error: None, message: None };
        assert_eq!(s.feed("zcash:t1abc"), "ignored");
        assert_eq!(s.feed("UR:LEEK-PCZT/AEAEAEAE"), "ignored");
        assert_eq!(s.feed("UR:CRYPTO-PSBT/AEAEAEAE"), "rejected");
        assert!(s.error().unwrap().contains("ur:crypto-psbt"));
        let mut done = false;
        for p in fixture.lines().filter(|l| !l.starts_with('#') && !l.is_empty()).rev() {
            if s.feed(&p.to_uppercase()) == "complete" {
                done = true;
                break;
            }
        }
        assert!(done);
        assert_eq!(s.take_message().unwrap(), zsig);
        assert_eq!(s.total(), 0, "starts over");

        let mut a = UrScan { d: Decoder::new(), want: Want::Accounts, error: None, message: None };
        assert_eq!(a.feed("ur:leek-zsig/aeaeaeae"), "ignored");
        let rec = Encoder::new(UR_ACCOUNTS, &[0xa2, 1, 2, 3, 4], 50).unwrap().frame(0);
        assert_eq!(a.feed(&rec), "complete");
        assert_eq!(a.take_message().unwrap(), vec![0xa2, 1, 2, 3, 4]);
    }

    #[test]
    fn the_hints_are_the_laptop_ones() {
        let h = display_hints();
        assert_eq!((h.fragment_len, h.frame_ms, h.board_zoom, h.quiet_zone_modules), (50, 300, 2, 2));
        assert!(large_screen_tips().iter().any(|t| t.contains("2x")));
    }
}
