//! apply_zsig: a `.zsig` goes back into the wallet's PCZT only if it names
//! the exact bytes sent, they are the same transaction, and every signature
//! verifies; the result proves and extracts.

mod common;

use common::*;
use leek_device::zsig::{Entry, Pool, Zsig};
use leek_zcash::{apply_zsig, signer_view_bytes, LeekError, Network, SignerView};

#[test]
fn the_vector_zsig_applies_and_the_transaction_verifies() {
    let full = v("iw_send_shielded");
    let zsig = std::fs::read(repo().join("vectors/zsig/iw_send_shielded.zsig")).unwrap();
    let signed = apply_zsig(&parse(&full), &full, &zsig).expect("applies");
    proves_and_extracts(signed);
}

#[test]
fn signatures_over_either_view_apply_to_the_full_pczt() {
    for name in ["iw_send_shielded", "orchard_to_ironwood", "t_shield", "t_mixed", "t_send_tovk"] {
        let pczt = parse(&v(name));
        for view in [SignerView::Full, SignerView::Compact] {
            let sent = signer_view_bytes(&pczt, view).unwrap();
            let zsig = soft_sign(&sent, Network::Test);
            let signed = apply_zsig(&pczt, &sent, &zsig).unwrap_or_else(|e| panic!("{name} {view:?}: {e}"));
            proves_and_extracts(signed);
        }
    }
}

#[test]
fn the_compact_view_is_the_one_the_app_sends() {
    // Upstream's redaction reproduces the compact vectors byte for byte
    // (leek-android's hand-written compact_view made them).
    for name in ["iw_send_transparent", "iw_send_nu7", "orchard_to_ironwood", "t_shield", "t_mixed"] {
        let compact = signer_view_bytes(&parse(&v(name)), SignerView::Compact).unwrap();
        assert_eq!(compact, v(&format!("{name}_compact")), "{name}");
    }
}

#[test]
fn a_zsig_for_other_bytes_is_refused() {
    let pczt = parse(&v("iw_send_shielded"));
    let compact = signer_view_bytes(&pczt, SignerView::Compact).unwrap();
    let full = v("iw_send_shielded");
    let zsig = soft_sign(&compact, Network::Test);
    // Signed the compact view; named as if the full bytes were sent.
    assert_eq!(apply_zsig(&pczt, &full, &zsig).unwrap_err(), LeekError::ZsigHashMismatch);
}

#[test]
fn signatures_over_another_transaction_are_refused() {
    // The board would sign this (it shows the changed memo); the host must
    // not put those signatures into its own transaction.
    let pczt = parse(&v("iw_send_shielded"));
    let changed = v("iw_send_shielded_compact_memo_changed");
    let zsig = soft_sign(&changed, Network::Test);
    assert_eq!(apply_zsig(&pczt, &changed, &zsig).unwrap_err(), LeekError::SighashMismatch);
}

#[test]
fn a_bad_signature_is_refused() {
    let full = v("iw_send_shielded");
    let pczt = parse(&full);
    let mut zsig = std::fs::read(repo().join("vectors/zsig/iw_send_shielded.zsig")).unwrap();
    let n = zsig.len();
    zsig[n - 10] ^= 1;
    assert_eq!(apply_zsig(&pczt, &full, &zsig).unwrap_err(), LeekError::SignatureInvalid);

    // A transparent one: a flipped DER byte, then another key.
    let full = v("t_shield");
    let pczt = parse(&full);
    let zsig = soft_sign(&full, Network::Test);
    assert_eq!(zsig[39], 0, "first entry is transparent");
    let l = zsig[44] as usize;
    let mut bad = zsig.clone();
    bad[45 + l - 3] ^= 1;
    assert_eq!(apply_zsig(&pczt, &full, &bad).unwrap_err(), LeekError::SignatureInvalid);
    let mut bad = zsig.clone();
    bad[45 + l + 5] ^= 1;
    assert_eq!(apply_zsig(&pczt, &full, &bad).unwrap_err(), LeekError::SignatureInvalid);
    // A sighash type other than ALL.
    let mut bad = zsig;
    bad[45 + l - 1] = 0x02;
    assert_eq!(apply_zsig(&pczt, &full, &bad).unwrap_err(), LeekError::SignatureInvalid);
}

#[test]
fn a_signature_for_the_wrong_pool_or_action_is_refused() {
    // Orchard spend + Ironwood change: both bundles have actions.
    let full = v("orchard_to_ironwood");
    let pczt = parse(&full);
    let z = Zsig::parse(&soft_sign(&full, Network::Test)).unwrap();
    assert!(z.entries.iter().any(|e| e.pool == Pool::Orchard));
    for (i, e) in z.entries.iter().enumerate() {
        let mut moved = z.clone();
        moved.entries[i].pool = if e.pool == Pool::Orchard { Pool::Ironwood } else { Pool::Orchard };
        let err = apply_zsig(&pczt, &full, &moved.to_bytes().unwrap()).unwrap_err();
        assert_eq!(err, LeekError::SignatureInvalid, "entry {i} moved to the other pool");
        let mut moved = z.clone();
        moved.entries[i].index += 1;
        let err = apply_zsig(&pczt, &full, &moved.to_bytes().unwrap()).unwrap_err();
        assert_eq!(err, LeekError::SignatureInvalid, "entry {i} moved to another action");
    }
    // Ironwood only: an Orchard entry points at nothing.
    let full = v("iw_send_shielded");
    let mut zsig = std::fs::read(repo().join("vectors/zsig/iw_send_shielded.zsig")).unwrap();
    assert_eq!(zsig[39], 2);
    zsig[39] = 1;
    assert_eq!(apply_zsig(&parse(&full), &full, &zsig).unwrap_err(), LeekError::SignatureInvalid);
}

#[test]
fn duplicates_and_missing_signatures_are_refused() {
    let full = v("iw_send_shielded");
    let pczt = parse(&full);
    let z = Zsig::parse(&std::fs::read(repo().join("vectors/zsig/iw_send_shielded.zsig")).unwrap()).unwrap();
    let mut twice = z.clone();
    twice.entries.push(z.entries[0].clone());
    assert_eq!(apply_zsig(&pczt, &full, &twice.to_bytes().unwrap()).unwrap_err(), LeekError::SignatureInvalid);
    let none = Zsig::new(&full, vec![]);
    assert_eq!(apply_zsig(&pczt, &full, &none.to_bytes().unwrap()).unwrap_err(), LeekError::SignatureMissing);

    // Two transparent inputs, only one signed.
    let full = v("t_shield");
    let mut z = Zsig::parse(&soft_sign(&full, Network::Test)).unwrap();
    z.entries.retain(|e| !(e.pool == Pool::Transparent && e.index == 1));
    assert_eq!(apply_zsig(&parse(&full), &full, &z.to_bytes().unwrap()).unwrap_err(), LeekError::SignatureMissing);
}

#[test]
fn malformed_input_is_typed() {
    let full = v("iw_send_shielded");
    let pczt = parse(&full);
    assert_eq!(apply_zsig(&pczt, &full, b"ZSIG").unwrap_err(), LeekError::ZsigMalformed);
    assert_eq!(apply_zsig(&pczt, &full, b"nope").unwrap_err(), LeekError::ZsigMalformed);
    // A transparent entry in a version 1 file.
    let tz = Zsig::new(&full, vec![Entry::transparent(0, vec![0x30; 70], [2; 33])]);
    let mut bytes = tz.to_bytes().unwrap();
    bytes[4] = 1;
    assert_eq!(apply_zsig(&pczt, &full, &bytes).unwrap_err(), LeekError::ZsigMalformed);
    // Sent bytes in an unknown PCZT version.
    let mut future = full.clone();
    future[4] = 9;
    let z = Zsig::new(&future, vec![]).to_bytes().unwrap();
    assert_eq!(apply_zsig(&pczt, &future, &z).unwrap_err(), LeekError::PcztVersion);
}

#[test]
fn errors_never_echo_the_transaction() {
    let full = v("iw_send_shielded");
    let addr = vector_entry("iw_send_shielded")["outputs"][0]["address"].as_str().unwrap().to_string();
    let pczt = parse(&full);
    let mut zsig = std::fs::read(repo().join("vectors/zsig/iw_send_shielded.zsig")).unwrap();
    zsig[50] ^= 1;
    let msg = apply_zsig(&pczt, &full, &zsig).unwrap_err().to_string();
    assert!(!msg.contains(&addr) && !msg.contains("400000") && !msg.contains("0.004"), "{msg}");
}
