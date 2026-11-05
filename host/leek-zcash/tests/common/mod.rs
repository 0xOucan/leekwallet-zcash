//! Shared fixtures: the vectors, the abandon-art accounts, the soft signer
//! (which returns exactly what the board returns), and proving.

#![allow(dead_code)]

use std::path::PathBuf;

use leek_zcash::pczt::Pczt;
use leek_zcash::{AccountMeta, Network};
use serde_json::Value;

pub const ABANDON_ART: &str = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                               abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                               abandon art";

pub fn repo() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

/// A vector PCZT's bytes, by name (vectors/pczt.json).
pub fn v(name: &str) -> Vec<u8> {
    std::fs::read(repo().join("vectors/pczt").join(format!("{name}.pczt"))).unwrap()
}

pub fn vector_entry(name: &str) -> Value {
    let all: Vec<Value> = serde_json::from_slice(&std::fs::read(repo().join("vectors/pczt.json")).unwrap()).unwrap();
    all.into_iter().find(|e| e["name"] == name).expect("vector")
}

pub fn parse(bytes: &[u8]) -> Pczt {
    Pczt::parse(bytes).unwrap()
}

pub fn key(name: &str) -> Value {
    let all: Vec<Value> = serde_json::from_slice(&std::fs::read(repo().join("vectors/keys.json")).unwrap()).unwrap();
    all.into_iter().find(|v| v["name"] == name).expect("key vector")
}

/// The account of a keys.json entry ("test/abandon-art/pp0/acct0").
pub fn meta(name: &str) -> AccountMeta {
    let k = key(name);
    let network = if k["network"] == "main" { Network::Main } else { Network::Test };
    AccountMeta {
        seed_fingerprint: hex::decode(k["seed_fingerprint"].as_str().unwrap()).unwrap().try_into().unwrap(),
        index: k["account"].as_u64().unwrap() as u32,
        network,
        ufvk: Some(k["ufvk"].as_str().unwrap().to_string()),
    }
}

pub fn test0() -> AccountMeta {
    meta("test/abandon-art/pp0/acct0")
}

/// Signs `sent` as the board would, with the vectors' seed: a `.zsig`.
pub fn soft_sign(sent: &[u8], network: Network) -> Vec<u8> {
    let acct = match network {
        Network::Test => zecz::keys::DevAccount::from_mnemonic(&zcash_protocol::consensus::TEST_NETWORK, ABANDON_ART, "", 0),
        Network::Main => zecz::keys::DevAccount::from_mnemonic(&zcash_protocol::consensus::MAIN_NETWORK, ABANDON_ART, "", 0),
    }
    .unwrap();
    zecz::soft_signer::sign(sent, &acct, network.coin_type()).unwrap().to_bytes().unwrap()
}

/// Proves, finalizes and extracts a signed PCZT: extraction verifies every
/// proof and signature, as a wallet's own extract step does.
pub fn proves_and_extracts(signed: Pczt) {
    use rand::{rand_core::UnwrapErr, rngs::SysRng};
    let proven = zecz::authorize::prove(signed).unwrap();
    let finalized = pczt::roles::spend_finalizer::SpendFinalizer::new(proven).finalize_spends().unwrap();
    pczt::roles::tx_extractor::TransactionExtractor::new(finalized)
        .extract(UnwrapErr(SysRng))
        .expect("the extracted transaction's proofs and signatures verify");
}
