//! The SD-card air gap and persisting a signed result: the file the board
//! reads, its `.zsig` back into a SignResult, the result as bytes and back,
//! and comparing what was mined.

mod common;

use common::*;
use leek_device::zsig::sha256;
use leek_zcash::pczt::Pczt;
use leek_zcash::{
    check_mined, finish_sd_sign, prepare_sd_sign, signer_view_bytes, tx_txid, LeekError, MinedCheck, Network,
    SignResult, SignerView, SD_UNSIGNED_PCZT,
};

/// Proves and extracts like a wallet, returning the transaction's bytes and
/// txid. Each extraction draws a fresh binding signature.
fn extract(signed: Pczt) -> (Vec<u8>, [u8; 32]) {
    use rand::{rand_core::UnwrapErr, rngs::SysRng};
    let proven = zecz::authorize::prove(signed).unwrap();
    let finalized = pczt::roles::spend_finalizer::SpendFinalizer::new(proven).finalize_spends().unwrap();
    let tx = pczt::roles::tx_extractor::TransactionExtractor::new(finalized).extract(UnwrapErr(SysRng)).unwrap();
    let mut bytes = Vec::new();
    tx.write(&mut bytes).unwrap();
    (bytes, *tx.txid().as_ref())
}

#[test]
fn the_card_file_is_the_signer_view_and_its_zsig_applies() {
    for (name, view) in [
        ("iw_send_shielded", SignerView::Full),
        ("t_mixed", SignerView::Compact),
        ("t_shield", SignerView::Full),
    ] {
        let pczt = parse(&v(name));
        let req = prepare_sd_sign(&pczt, &test0(), view).unwrap();
        assert_eq!(req.file_name, SD_UNSIGNED_PCZT);
        assert_eq!(req.file_name, "LEEKZ/unsigned.pczt");
        assert_eq!(req.bytes, signer_view_bytes(&pczt, view).unwrap());
        assert_eq!(req.sent_hash, sha256(&req.bytes));
        assert_eq!((req.account, req.network), (0, Network::Test));

        // The board writes LEEKZ/signed.zsig: the soft signer returns the same.
        let zsig = soft_sign(&req.bytes, Network::Test);
        assert_eq!(&zsig[5..37], &req.sent_hash);
        let r = finish_sd_sign(&pczt, &req, &zsig).unwrap_or_else(|e| panic!("{name}: {e}"));
        assert_eq!(hex::encode(r.sighash), vector_entry(name)["sighash"].as_str().unwrap(), "{name}");
        assert_eq!(r.zsig, zsig);
        assert!(!r.signatures.is_empty());
        // The bytes alone do as well as the request.
        finish_sd_sign(&pczt, &req.bytes, &zsig).unwrap();

        let (tx, txid) = extract(r.signed_pczt.clone());
        assert_eq!(r.txid, txid, "{name}: the txid is final before proving");
        assert_eq!(tx_txid(&tx).unwrap(), txid);
    }
}

#[test]
fn a_stale_or_foreign_card_is_refused() {
    let pczt = parse(&v("iw_send_shielded"));
    let req = prepare_sd_sign(&pczt, &test0(), SignerView::Full).unwrap();
    // A .zsig left from another transaction.
    let other = prepare_sd_sign(&parse(&v("t_shield")), &test0(), SignerView::Full).unwrap();
    let old = soft_sign(&other.bytes, Network::Test);
    assert_eq!(finish_sd_sign(&pczt, &req, &old).unwrap_err(), LeekError::ZsigHashMismatch);
    // The right .zsig, but the wallet hands in another view than it wrote.
    let zsig = soft_sign(&req.bytes, Network::Test);
    let compact = signer_view_bytes(&pczt, SignerView::Compact).unwrap();
    assert_eq!(finish_sd_sign(&pczt, &compact, &zsig).unwrap_err(), LeekError::ZsigHashMismatch);
    // Signatures for this file applied to another transaction.
    assert_eq!(
        finish_sd_sign(&parse(&v("t_shield")), &req, &zsig).unwrap_err(),
        LeekError::SighashMismatch
    );
    assert_eq!(finish_sd_sign(&pczt, &req, b"ZSIG").unwrap_err(), LeekError::ZsigMalformed);
}

#[test]
fn the_card_file_is_checked_before_it_is_written() {
    let pczt = parse(&v("iw_send_shielded"));
    // Another seed: the spends and inputs name a different fingerprint.
    let mut other = test0();
    other.seed_fingerprint = [1; 32];
    assert!(prepare_sd_sign(&pczt, &other, SignerView::Full).is_err());
    let mut main = test0();
    main.network = Network::Main;
    main.ufvk = None;
    assert_eq!(prepare_sd_sign(&pczt, &main, SignerView::Full).unwrap_err(), LeekError::WrongNetwork);
}

#[test]
fn a_sign_result_persists_and_reads_back() {
    let pczt = parse(&v("t_mixed"));
    let req = prepare_sd_sign(&pczt, &test0(), SignerView::Compact).unwrap();
    let r = finish_sd_sign(&pczt, &req, &soft_sign(&req.bytes, Network::Test)).unwrap();
    let bytes = r.to_bytes().unwrap();
    assert_eq!(&bytes[..5], b"LKSR\x01");
    let back = SignResult::from_bytes(&bytes).unwrap();
    assert_eq!(back.txid, r.txid);
    assert_eq!(back.txid_hex(), r.txid_hex());
    assert_eq!(back.sighash, r.sighash);
    assert_eq!(back.zsig, r.zsig);
    assert_eq!(back.signatures, r.signatures);
    assert_eq!(back.signed_pczt.clone().serialize().unwrap(), r.signed_pczt.clone().serialize().unwrap());
    assert_eq!(back.to_bytes().unwrap(), bytes);
    // The display form is the byte-reversed hex.
    let mut rev = r.txid;
    rev.reverse();
    assert_eq!(r.txid_hex(), hex::encode(rev));

    // Damage anywhere is refused, never read as another txid.
    for at in [0, 4, 6, 40, 70, bytes.len() / 2, bytes.len() - 1] {
        let mut bad = bytes.clone();
        bad[at] ^= 1;
        assert!(SignResult::from_bytes(&bad).is_err(), "byte {at}");
    }
    assert!(SignResult::from_bytes(&bytes[..bytes.len() - 1]).is_err());
}

#[test]
fn what_was_mined_is_compared() {
    let pczt = parse(&v("iw_send_shielded"));
    let req = prepare_sd_sign(&pczt, &test0(), SignerView::Full).unwrap();
    let r = finish_sd_sign(&pczt, &req, &soft_sign(&req.bytes, Network::Test)).unwrap();
    let (tx, txid) = extract(r.signed_pczt.clone());
    assert_eq!(check_mined(&tx, &tx).unwrap(), MinedCheck::Identical);
    // Extracted again: same effects, another binding signature and proofs.
    let (again, txid2) = extract(r.signed_pczt);
    assert_eq!(txid, txid2);
    assert_ne!(tx, again);
    assert_eq!(check_mined(&tx, &again).unwrap(), MinedCheck::SameEffectsOtherAuth);
    // Another transaction.
    let other = parse(&v("t_shield"));
    let req = prepare_sd_sign(&other, &test0(), SignerView::Full).unwrap();
    let o = finish_sd_sign(&other, &req, &soft_sign(&req.bytes, Network::Test)).unwrap();
    let (other_tx, _) = extract(o.signed_pczt);
    assert_eq!(check_mined(&tx, &other_tx).unwrap(), MinedCheck::Different);
    assert_eq!(check_mined(&tx, b"junk").unwrap_err(), LeekError::Parse);
}
