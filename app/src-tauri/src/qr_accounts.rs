//! The board's account off its "View key" QR: an air-gapped
//! wallet set-up, for a companion that has never reached the board over USB
//! or Bluetooth.
//!
//! On the board, Main menu -> View key, after a confirmation there, shows
//! an animated `ur:leek-zcash-accounts` ([`ACCOUNT_RECORD_UR_TYPE`]): the
//! CBOR of an [`AccountRecord`] (host/leek-zcash/src/record.rs) as the UR
//! message itself, not wrapped in a byte string. The board sends one
//! account, without a name or a birthday (it does not know chain heights).
//! The camera's codes come here one at a time, the same transport as the
//! board's `ur:leek-zsig` reply (src/airgap.rs), on a decoder of their own.
//!
//! What comes back is what `zw_import` takes for a board account: the seed
//! fingerprint, the account index, the UFVK, and the default unified
//! address (diversifier index 0, Orchard + P2PKH, the one the board's
//! `zcash_account` derives) computed here from the UFVK, so nothing but
//! the viewing key itself is taken on trust. The result is a normal board
//! wallet, signable over QR, USB or Bluetooth, never a watch-only one.
//!
//! One way only: this reads a viewing key, it never pairs. USB and
//! Bluetooth keep their 6-digit pairing code.

use std::sync::Mutex;

use leek_ur::{Decoder, Received, UrError, UR_PCZT, UR_ZSIG};
use leek_zcash::{AccountRecord, ACCOUNT_RECORD_UR_TYPE};
use serde::Serialize;
use tauri::State;
use zecz::wallet;

use crate::zwallet::parse_net;

/// The account record being assembled.
#[derive(Default)]
pub struct QrAccountsState(Mutex<Decoder>);

/// One account of the record, ready for `zw_import`.
#[derive(Clone, Debug, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct QrAccount {
    pub ufvk: String,
    /// The ZIP 32 account index.
    pub account: u32,
    /// The default unified address, derived here from `ufvk`.
    pub ua: String,
    /// Set only by a signer that knows them; the board sends neither.
    pub name: Option<String>,
    pub birthday: Option<u32>,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct QrAccountRecord {
    /// "test" or "main", as the other commands take it.
    pub network: String,
    /// 64 hex digits, lowercase: the `seedFingerprint` of `zw_import`.
    pub seed_fingerprint: String,
    /// The first 8 hex digits, uppercase: the "ZFP" the app shows.
    pub zfp: String,
    pub accounts: Vec<QrAccount>,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct AccountsProgress {
    /// "accepted", "redundant", "ignored" (not a UR, or the app's own
    /// ur:leek-pczt / the board's ur:leek-zsig), "rejected" (a broken or
    /// foreign part; scanning goes on), "refused" (assembled, but not an
    /// account this app can import: another network, not a board account,
    /// a malformed record; scanning stops) or "complete".
    pub state: &'static str,
    pub received: u32,
    /// 0 until the first ur:leek-zcash-accounts part.
    pub total: u32,
    pub error: Option<String>,
    /// The record, once complete.
    pub record: Option<QrAccountRecord>,
}

/// The record as `network`'s wallet would import it, or why it cannot.
pub fn check_record(net: zecz::Network, message: &[u8]) -> Result<QrAccountRecord, String> {
    let r = AccountRecord::decode(message).map_err(|e| format!("not a readable account record: {e}"))?;
    if r.accounts.is_empty() {
        return Err("the account record holds no account".into());
    }
    let want = match net {
        zecz::Network::MainNetwork => "main",
        zecz::Network::TestNetwork => "test",
    };
    let mut accounts = Vec::with_capacity(r.accounts.len());
    for a in &r.accounts {
        let theirs = a.network().map_err(|e| format!("the viewing key: {e}"))?;
        if theirs.as_str() != want {
            return Err(format!(
                "this is a {} viewing key, and the {} wallet is chosen here: pick {} and scan again",
                label(theirs.as_str()),
                label(want),
                label(theirs.as_str()),
            ));
        }
        // The board account's shape: Orchard + P2PKH, no Sapling.
        wallet::parse_ufvk(net, &a.ufvk).map_err(|e| format!("not a LeekWallet board account: {e:#}"))?;
        let ua = wallet::addresses_for_ufvk(net, &a.ufvk).map_err(|e| format!("{e:#}"))?.unified;
        accounts.push(QrAccount { ufvk: a.ufvk.clone(), account: a.index, ua, name: a.name.clone(), birthday: a.birthday });
    }
    let seed_fingerprint = hex::encode(r.seed_fingerprint);
    Ok(QrAccountRecord {
        network: want.into(),
        zfp: seed_fingerprint[..8].to_ascii_uppercase(),
        seed_fingerprint,
        accounts,
    })
}

fn label(net: &str) -> &'static str {
    if net == "main" {
        "Mainnet"
    } else {
        "Testnet"
    }
}

pub fn feed(d: &mut Decoder, net: zecz::Network, text: &str) -> AccountsProgress {
    let text = text.trim();
    let progress = |d: &Decoder, state, error: Option<String>, record| AccountsProgress {
        state,
        received: d.received(),
        total: d.seq_len(),
        error,
        record,
    };
    match leek_ur::part_type(text).as_deref() {
        Some(t) if t == ACCOUNT_RECORD_UR_TYPE => {}
        Some(UR_PCZT) | Some(UR_ZSIG) | None => return progress(d, "ignored", None, None),
        Some(other) => {
            return progress(d, "rejected", Some(format!("a ur:{other}, not the board's ur:{ACCOUNT_RECORD_UR_TYPE}")), None)
        }
    }
    match d.receive(text) {
        Ok(Received::Complete) => {
            let r = check_record(net, d.message().expect("complete"));
            d.reset();
            match r {
                Ok(rec) => progress(d, "complete", None, Some(rec)),
                Err(e) => progress(d, "refused", Some(e), None),
            }
        }
        Ok(Received::Accepted) => progress(d, "accepted", None, None),
        Ok(Received::Redundant) => progress(d, "redundant", None, None),
        Err(e @ UrError::Checksum) => progress(d, "rejected", Some(format!("{e}; starting over")), None),
        Err(e) => progress(d, "rejected", Some(e.to_string()), None),
    }
}

/// Forget any partial record: the import screen calls this before scanning.
#[tauri::command]
pub fn qr_accounts_reset(state: State<'_, QrAccountsState>) {
    state.0.lock().unwrap_or_else(|p| p.into_inner()).reset();
}

/// One decoded QR text from the camera, for the wallet of `network`.
#[tauri::command]
pub fn qr_accounts_feed(state: State<'_, QrAccountsState>, network: String, text: String) -> Result<AccountsProgress, String> {
    let net = parse_net(&network)?;
    let mut d = state.0.lock().unwrap_or_else(|p| p.into_inner());
    Ok(feed(&mut d, net, &text))
}

#[cfg(test)]
mod tests {
    use super::*;
    use leek_ur::Encoder;
    use leek_zcash::AccountEntry;

    /// The first account-0 vector of `net`: (ufvk, ua, seed fingerprint).
    fn vector(net: &str) -> (String, String, [u8; 32]) {
        let keys: Vec<serde_json::Value> = serde_json::from_str(
            &std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/../../vectors/keys.json")).unwrap(),
        )
        .unwrap();
        let k = keys.iter().find(|k| k["network"] == net && k["account"] == 0).unwrap();
        let fp = hex::decode(k["seed_fingerprint"].as_str().unwrap()).unwrap().try_into().unwrap();
        (k["ufvk"].as_str().unwrap().into(), k["ua"].as_str().unwrap().into(), fp)
    }

    /// What the board shows: one account, no name, no birthday.
    fn board_record(net: &str) -> (Vec<u8>, String, [u8; 32]) {
        let (ufvk, ua, fp) = vector(net);
        let r = AccountRecord { seed_fingerprint: fp, accounts: vec![AccountEntry { ufvk, index: 0, name: None, birthday: None }] };
        (r.encode().unwrap(), ua, fp)
    }

    fn parts(message: &[u8], fragment: usize) -> Vec<String> {
        let e = Encoder::new(ACCOUNT_RECORD_UR_TYPE, message, fragment).unwrap();
        // Twice round the loop: the fountain's mixed parts too.
        (0..e.seq_len() as u64 * 2).map(|i| e.frame(i)).collect()
    }

    fn scan(d: &mut Decoder, net: zecz::Network, frames: &[String]) -> AccountsProgress {
        for f in frames {
            let p = feed(d, net, f);
            if p.state != "accepted" && p.state != "redundant" {
                return p;
            }
        }
        panic!("never complete");
    }

    #[test]
    fn the_board_record_assembles_with_its_address() {
        let (msg, ua, fp) = board_record("test");
        let mut frames = parts(&msg, 50);
        assert!(frames.len() > 4, "multi-part");
        frames.reverse();
        let mut d = Decoder::new();
        // Lowercase, as a camera may read it.
        let lower: Vec<String> = frames.iter().map(|f| f.to_ascii_lowercase()).collect();
        let p = scan(&mut d, zecz::Network::TestNetwork, &lower);
        assert_eq!(p.state, "complete", "{p:?}");
        let r = p.record.unwrap();
        assert_eq!(r.network, "test");
        assert_eq!(r.seed_fingerprint, hex::encode(fp));
        assert_eq!(r.zfp, hex::encode(&fp[..4]).to_uppercase());
        assert_eq!(r.accounts.len(), 1);
        let a = &r.accounts[0];
        // The address the board itself shows for that key.
        assert_eq!((a.account, a.ua.as_str(), a.name.as_ref(), a.birthday), (0, ua.as_str(), None, None));
        // The decoder starts over for the next import.
        assert_eq!(d.seq_len(), 0);
        // And uppercase works the same.
        let mut d = Decoder::new();
        assert_eq!(scan(&mut d, zecz::Network::TestNetwork, &frames).state, "complete");
    }

    #[test]
    fn other_codes_are_ignored_or_rejected() {
        let (msg, _, _) = board_record("main");
        let mut d = Decoder::new();
        let main = zecz::Network::MainNetwork;
        assert_eq!(feed(&mut d, main, "zcash:u1abc").state, "ignored");
        // The app's own transaction loop in a mirror, the signature reply.
        assert_eq!(feed(&mut d, main, "UR:LEEK-PCZT/1-28/AEAEAEAE").state, "ignored");
        let pczt = Encoder::leek_pczt(b"PCZT\x01\x02\x03\x04", 50).unwrap().frame(0);
        assert_eq!(feed(&mut d, main, &pczt).state, "ignored");
        assert_eq!(feed(&mut d, main, "ur:leek-zsig/aeaeaeae").state, "ignored");
        // Another wallet's type: rejected, and nothing pinned.
        let foreign = Encoder::new("zcash-accounts", &msg, 50).unwrap().frame(0);
        let p = feed(&mut d, main, &foreign);
        assert_eq!(p.state, "rejected");
        assert!(p.error.unwrap().contains("ur:zcash-accounts"));
        assert_eq!(d.seq_len(), 0);
        // A broken part of the right type: rejected, scanning goes on.
        assert_eq!(feed(&mut d, main, "UR:LEEK-ZCASH-ACCOUNTS/1-3/NOTBYTEWORDS").state, "rejected");
        let p = scan(&mut d, main, &parts(&msg, 50));
        assert_eq!(p.state, "complete");
    }

    #[test]
    fn a_key_for_the_other_network_is_refused() {
        let (msg, _, _) = board_record("main");
        let mut d = Decoder::new();
        let p = scan(&mut d, zecz::Network::TestNetwork, &parts(&msg, 50));
        assert_eq!(p.state, "refused");
        assert!(p.record.is_none());
        let e = p.error.unwrap();
        assert!(e.contains("Mainnet viewing key") && e.contains("Testnet wallet"), "{e}");
        assert_eq!(d.seq_len(), 0, "starts over");
    }

    #[test]
    fn a_record_that_is_not_a_board_account_is_refused() {
        let net = zecz::Network::TestNetwork;
        // Not the record's CBOR (a byte string around it, as a zsig is).
        let (msg, _, _) = board_record("test");
        let wrapped = leek_ur::wrap_bytes(&msg);
        assert!(check_record(net, &wrapped).unwrap_err().contains("account record"));
        let empty = AccountRecord { seed_fingerprint: [1; 32], accounts: vec![] }.encode().unwrap();
        assert!(check_record(net, &empty).is_err());
        // As a single-part code, refused the same way.
        let mut d = Decoder::new();
        let one = leek_ur::encode_single(ACCOUNT_RECORD_UR_TYPE, &wrapped).unwrap();
        assert_eq!(feed(&mut d, net, &one).state, "refused");
        // And the board's record, single-part, is fine.
        assert_eq!(feed(&mut d, net, &leek_ur::encode_single(ACCOUNT_RECORD_UR_TYPE, &msg).unwrap()).state, "complete");
    }

    #[test]
    fn the_progress_serializes_as_the_webview_reads_it() {
        let (msg, _, _) = board_record("test");
        let mut d = Decoder::new();
        let p = scan(&mut d, zecz::Network::TestNetwork, &parts(&msg, 500));
        let v = serde_json::to_value(&p).unwrap();
        assert_eq!(v["state"], "complete");
        assert!(v["record"]["seedFingerprint"].is_string() && v["record"]["zfp"].is_string());
        let a = &v["record"]["accounts"][0];
        assert!(a["ufvk"].is_string() && a["ua"].is_string() && a["account"] == 0 && a["birthday"].is_null());
    }
}
