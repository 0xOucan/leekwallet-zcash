//! Offline check of the dev soft-signer path against the committed PCZT
//! vectors: sign -> .zsig -> parse -> re-apply with the Signer role, which
//! verifies every signature against its action's rk.

use std::path::PathBuf;

use pczt::roles::signer::Signer;
use pczt::Pczt;
use serde_json::Value;
use zcash_protocol::consensus::{NetworkConstants, TestNetwork};
use zecz::keys::DevAccount;
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
    soft_signer::apply(pczt.clone(), &parsed).unwrap();

    // A corrupted signature must be rejected by rk verification.
    let mut bad = parsed.clone();
    bad.entries[0].sig[40] ^= 1;
    assert!(soft_signer::apply(pczt, &bad).is_err());

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
