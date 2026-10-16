//! The Android signature step against the vectors: a .zsig from the soft
//! signer applies to its PCZT, and the result proves and extracts into a
//! transaction whose signatures verify - the same check `zecz finish` makes.
//! Anything else is refused.

use std::path::PathBuf;

use pczt::Pczt;
use leek_android::apply_signatures;

fn v(p: &str) -> Vec<u8> {
    std::fs::read(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../vectors").join(p)).unwrap()
}

#[test]
fn signatures_apply_and_the_transaction_verifies() {
    use rand::{rand_core::UnwrapErr, rngs::SysRng};
    let pczt = v("pczt/iw_send_shielded.pczt");
    let signed = apply_signatures(&pczt, &v("zsig/iw_send_shielded.zsig")).expect("applies");
    let proven = zecz::authorize::prove(Pczt::parse(&signed).unwrap()).unwrap();
    let finalized = pczt::roles::spend_finalizer::SpendFinalizer::new(proven).finalize_spends().unwrap();
    pczt::roles::tx_extractor::TransactionExtractor::new(finalized)
        .extract(UnwrapErr(SysRng))
        .expect("the extracted transaction's signatures verify");
}

#[test]
fn a_zsig_for_other_bytes_is_refused() {
    let mut pczt = v("pczt/iw_send_shielded.pczt");
    let zsig = v("zsig/iw_send_shielded.zsig");
    let last = pczt.len() - 1;
    pczt[last] ^= 1;
    let err = apply_signatures(&pczt, &zsig).unwrap_err();
    assert!(err.contains("different PCZT"), "{err}");
}

#[test]
fn a_bad_signature_is_refused() {
    let pczt = v("pczt/iw_send_shielded.pczt");
    let mut zsig = v("zsig/iw_send_shielded.zsig");
    let n = zsig.len();
    zsig[n - 10] ^= 1;
    assert!(apply_signatures(&pczt, &zsig).is_err(), "a corrupted signature applied");
    let mut wrong_index = v("zsig/iw_send_shielded.zsig");
    wrong_index[40] = 1; // action 1 is not ours / does not take this signature
    assert!(apply_signatures(&pczt, &wrong_index).is_err(), "a signature applied to another action");
    assert!(apply_signatures(&pczt, b"ZSIG").is_err(), "truncated");
}

/// Signs `sent` as the device would: with the vectors' sender seed.
fn soft_sign(sent: &[u8]) -> Vec<u8> {
    let params = zcash_protocol::consensus::TEST_NETWORK;
    let acct = zecz::keys::DevAccount::from_mnemonic(&params, SENDER, "", 0).unwrap();
    zecz::soft_signer::sign(sent, &acct, 1).unwrap().to_bytes().unwrap()
}

const SENDER: &str = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon art";

#[test]
fn the_compact_view_is_about_half_and_keeps_the_sighash() {
    use pczt::roles::signer::Signer;
    let full = v("pczt/iw_send_shielded.pczt");
    let compact = leek_android::compact_view(&full).unwrap();
    assert!(compact.len() * 10 < full.len() * 6, "compact {} B vs full {} B", compact.len(), full.len());
    assert_eq!(&compact[..8], b"PCZT\x02\x00\x00\x00", "v2 encoding");
    let sh = |b: &[u8]| Signer::new(Pczt::parse(b).unwrap()).unwrap().shielded_sighash();
    assert_eq!(sh(&compact), sh(&full));
    // Compacting twice changes nothing.
    assert_eq!(leek_android::compact_view(&compact).unwrap(), compact);
}

#[test]
fn signatures_on_the_compact_view_apply_to_the_full_pczt() {
    use rand::{rand_core::UnwrapErr, rngs::SysRng};
    let full = v("pczt/iw_send_shielded.pczt");
    let compact = leek_android::compact_view(&full).unwrap();
    let zsig = soft_sign(&compact);
    // The signatures name the compact bytes, not the full ones.
    assert!(apply_signatures(&full, &zsig).unwrap_err().contains("different PCZT"));
    let signed = leek_android::apply_signatures_to(&full, &compact, &zsig).expect("applies");
    let proven = zecz::authorize::prove(Pczt::parse(&signed).unwrap()).unwrap();
    let finalized = pczt::roles::spend_finalizer::SpendFinalizer::new(proven).finalize_spends().unwrap();
    pczt::roles::tx_extractor::TransactionExtractor::new(finalized)
        .extract(UnwrapErr(SysRng))
        .expect("the extracted transaction's signatures verify");
}

#[test]
fn a_compact_view_of_another_transaction_is_refused() {
    let full = v("pczt/iw_send_shielded.pczt");
    let other = leek_android::compact_view(&v("pczt/iw_send_nu7.pczt")).unwrap();
    let zsig = soft_sign(&other);
    let err = leek_android::apply_signatures_to(&full, &other, &zsig).unwrap_err();
    assert!(err.contains("different transaction"), "{err}");
}

#[test]
fn the_compact_vectors_are_what_the_app_sends() {
    use pczt::roles::signer::Signer;
    let sh = |b: &[u8]| Signer::new(Pczt::parse(b).unwrap()).unwrap().shielded_sighash();
    for name in ["iw_send_transparent", "iw_send_nu7", "orchard_to_ironwood"] {
        let full = v(&format!("pczt/{name}.pczt"));
        let compact = v(&format!("pczt/{name}_compact.pczt"));
        assert_eq!(leek_android::compact_view(&full).unwrap(), compact, "{name}");
        assert_eq!(sh(&compact), sh(&full), "{name}");
    }
}

#[test]
fn signatures_over_a_changed_memo_are_refused() {
    // The device accepts this one: it shows the changed memo and signs that
    // transaction. The host must not apply those signatures to its own.
    let full = v("pczt/iw_send_shielded.pczt");
    let changed = v("pczt/iw_send_shielded_compact_memo_changed.pczt");
    let zsig = soft_sign(&changed);
    let err = leek_android::apply_signatures_to(&full, &changed, &zsig).unwrap_err();
    assert!(err.contains("different transaction"), "{err}");
    // Named as the honest compact view, the hash check refuses it instead.
    let honest = leek_android::compact_view(&full).unwrap();
    let err = leek_android::apply_signatures_to(&full, &honest, &zsig).unwrap_err();
    assert!(err.contains("different PCZT"), "{err}");
}
