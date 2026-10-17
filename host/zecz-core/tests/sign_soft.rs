//! Offline check of the dev soft-signer path against the committed PCZT
//! vectors: sign -> .zsig -> parse -> re-apply with the Signer role, which
//! verifies every signature against its action's rk.

use std::path::PathBuf;

use pczt::roles::signer::Signer;
use pczt::Pczt;
use serde_json::Value;
use zcash_protocol::consensus::{NetworkConstants, TestNetwork};
use zecz::keys::DevAccount;
use zecz::authorize::{apply_zsig, prove};
use zecz::soft_signer;
use zecz::zsig::{Pool, Zsig, ZsigError};

fn vectors() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../vectors")
}

fn pczt_vector(name: &str) -> (Value, Vec<u8>) {
    let all: Vec<Value> = serde_json::from_slice(&std::fs::read(vectors().join("pczt.json")).unwrap()).unwrap();
    let v = all.into_iter().find(|v| v["name"] == name).expect("vector");
    let bytes = std::fs::read(vectors().join(v["file"].as_str().unwrap())).unwrap();
    (v, bytes)
}

fn signer_account(v: &Value) -> DevAccount {
    assert_eq!(v["network"], "test");
    DevAccount::from_mnemonic(
        &TestNetwork,
        v["signer_mnemonic"].as_str().unwrap(),
        v["signer_passphrase"].as_str().unwrap(),
        v["account"].as_u64().unwrap() as u32,
    )
    .unwrap()
}

#[test]
fn dev_keys_match_vectors() {
    let all: Vec<Value> = serde_json::from_slice(&std::fs::read(vectors().join("keys.json")).unwrap()).unwrap();
    let mut n = 0;
    for v in all.iter().filter(|v| v["network"] == "test") {
        let acct = DevAccount::from_mnemonic(
            &TestNetwork,
            v["mnemonic"].as_str().unwrap(),
            v["passphrase"].as_str().unwrap(),
            v["account"].as_u64().unwrap() as u32,
        )
        .unwrap();
        assert_eq!(acct.ufvk.encode(&TestNetwork).unwrap(), v["ufvk"].as_str().unwrap(), "{}", v["name"]);
        assert_eq!(hex::encode(acct.seed_fingerprint.to_bytes()), v["seed_fingerprint"].as_str().unwrap());
        n += 1;
    }
    assert!(n > 0);
}

#[test]
fn sign_soft_iw_send_shielded() {
    let (v, unsigned) = pczt_vector("iw_send_shielded");
    let acct = signer_account(&v);

    // Sign and serialize exactly as `zecz sign-soft` does.
    let zsig = soft_signer::sign(&unsigned, &acct, TestNetwork.coin_type()).unwrap();
    let zsig_bytes = zsig.to_bytes().unwrap();
    assert_eq!(&zsig_bytes[..5], b"ZSIG\x01");

    // What `zecz finish` does: parse, check the hash, then apply.
    let parsed = Zsig::parse_for(&zsig_bytes, &unsigned).unwrap();
    assert!(!parsed.entries.is_empty());
    assert!(parsed.entries.iter().all(|e| e.pool == Pool::Ironwood), "an NU6.3 send spends Ironwood");

    let pczt = Pczt::parse(&unsigned).unwrap();
    let sighash = Signer::new(pczt.clone()).unwrap().shielded_sighash();
    assert_eq!(hex::encode(sighash), v["sighash"].as_str().unwrap());

    let mut signer = Signer::new(pczt.clone()).unwrap();
    for e in &parsed.entries {
        let sig = orchard::primitives::redpallas::Signature::<orchard::primitives::redpallas::SpendAuth>::from(e.sig);
        signer.apply_ironwood_signature(e.index as usize, sig).expect("signature verifies against rk");
    }
    let signed = signer.finish();
    for e in &parsed.entries {
        assert_eq!(signed.ironwood().actions()[e.index as usize].spend().spend_auth_sig(), &Some(e.sig));
    }

    // The library helper applies the same way.
    apply_zsig(pczt.clone(), &parsed).unwrap();

    // A corrupted signature must be rejected by rk verification.
    let mut bad = parsed.clone();
    bad.entries[0].sig[40] ^= 1;
    assert!(apply_zsig(pczt, &bad).is_err());

    // A .zsig for these bytes must not be accepted for any other PCZT.
    let (_, other) = pczt_vector("iw_send_transparent");
    assert!(matches!(Zsig::parse_for(&zsig_bytes, &other), Err(ZsigError::HashMismatch { .. })));
}

#[test]
fn sign_soft_refuses_foreign_spend() {
    let (v, unsigned) = pczt_vector("r_foreign_spend");
    let acct = signer_account(&v);
    assert!(soft_signer::sign(&unsigned, &acct, TestNetwork.coin_type()).is_err());
}

#[test]
fn sign_soft_refuses_wrong_network() {
    let (v, unsigned) = pczt_vector("iw_send_shielded");
    let acct = signer_account(&v);
    assert!(soft_signer::sign(&unsigned, &acct, 133).is_err());
}

/// The `finish` path without a wallet: sign, apply, prove with the key for
/// the PCZT's branch, then finalize and extract a transaction that verifies.
#[test]
fn sign_apply_prove_extract_iw_send_shielded() {
    use rand::{rand_core::UnwrapErr, rngs::SysRng};

    let (v, unsigned) = pczt_vector("iw_send_shielded");
    let acct = signer_account(&v);
    let zsig = soft_signer::sign(&unsigned, &acct, TestNetwork.coin_type()).unwrap();
    let signed = apply_zsig(Pczt::parse(&unsigned).unwrap(), &zsig).unwrap();
    let proven = prove(signed).unwrap();
    assert!(proven.ironwood().zkproof().is_some());

    let finalized = pczt::roles::spend_finalizer::SpendFinalizer::new(proven).finalize_spends().unwrap();
    let tx = pczt::roles::tx_extractor::TransactionExtractor::new(finalized)
        .extract(UnwrapErr(SysRng))
        .expect("extraction verifies proofs and signatures");
    assert_eq!(u32::from(tx.consensus_branch_id()), 0x37a5_165b);
}

/// Roadmap 10.1: a PCZT with transparent inputs signs, applies (each
/// transparent signature checked against its input's own sighash), proves
/// and extracts into a transaction that verifies.
#[test]
fn transparent_inputs_sign_apply_prove_extract() {
    use rand::{rand_core::UnwrapErr, rngs::SysRng};

    for name in ["t_shield", "t_mixed", "t_send_tovk"] {
        let (v, unsigned) = pczt_vector(name);
        let acct = signer_account(&v);
        let zsig = soft_signer::sign(&unsigned, &acct, TestNetwork.coin_type()).unwrap();
        let bytes = zsig.to_bytes().unwrap();
        assert_eq!(bytes[4], 2, "{name}: a transparent .zsig is version 2");
        let parsed = Zsig::parse_for(&bytes, &unsigned).unwrap();
        let n_t = v["t_inputs"].as_array().unwrap().len();
        assert_eq!(parsed.entries.iter().filter(|e| e.pool == Pool::Transparent).count(), n_t, "{name}");

        let signed = apply_zsig(Pczt::parse(&unsigned).unwrap(), &parsed).expect("transparent signatures verify");
        let proven = prove(signed).unwrap();
        let finalized = pczt::roles::spend_finalizer::SpendFinalizer::new(proven).finalize_spends().unwrap();
        let tx = pczt::roles::tx_extractor::TransactionExtractor::new(finalized)
            .extract(UnwrapErr(SysRng))
            .expect("extraction verifies proofs and signatures");
        assert_eq!(tx.transparent_bundle().unwrap().vin.len(), n_t, "{name}");
    }
}

#[test]
fn a_bad_transparent_signature_is_refused() {
    let (v, unsigned) = pczt_vector("t_mixed");
    let acct = signer_account(&v);
    let zsig = soft_signer::sign(&unsigned, &acct, TestNetwork.coin_type()).unwrap();
    let pczt = Pczt::parse(&unsigned).unwrap();
    let t = zsig.entries.iter().position(|e| e.pool == Pool::Transparent).unwrap();

    // A flipped bit in s.
    let mut bad = zsig.clone();
    let s = bad.entries[t].transparent.as_mut().unwrap();
    let k = s.sig.len() - 3;
    s.sig[k] ^= 1;
    assert!(apply_zsig(pczt.clone(), &bad).is_err(), "corrupted signature applied");

    // A key that is not the one the script pays to.
    let mut bad = zsig.clone();
    bad.entries[t].transparent.as_mut().unwrap().pubkey[5] ^= 1;
    assert!(apply_zsig(pczt.clone(), &bad).is_err(), "signature under another key applied");

    // Another sighash type than SIGHASH_ALL.
    let mut bad = zsig.clone();
    *bad.entries[t].transparent.as_mut().unwrap().sig.last_mut().unwrap() = 0x02;
    assert!(apply_zsig(pczt.clone(), &bad).is_err(), "SIGHASH_NONE applied");

    // An input that does not exist.
    let mut bad = zsig;
    bad.entries[t].index = 7;
    assert!(apply_zsig(pczt, &bad).is_err(), "signature for a missing input applied");
}

#[test]
fn sign_soft_refuses_foreign_transparent_inputs() {
    for name in ["r_t_input_not_ours", "r_t_wrong_path", "r_t_wrong_account", "r_t_missing_derivation", "r_t_foreign_seed"] {
        let (v, unsigned) = pczt_vector(name);
        let acct = signer_account(&v);
        assert!(soft_signer::sign(&unsigned, &acct, TestNetwork.coin_type()).is_err(), "{name}");
    }
}
