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
