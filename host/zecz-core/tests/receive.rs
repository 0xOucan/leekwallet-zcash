//! Receive addresses without a network: rotating diversified addresses,
//! and the address check the desktop's contacts use.

use std::path::PathBuf;
use std::sync::atomic::{AtomicU32, Ordering};

use serde_json::Value;
use zcash_client_backend::data_api::chain::ChainState;
use zcash_keys::keys::{ReceiverRequirement::*, UnifiedAddressRequest, UnifiedFullViewingKey};
use zcash_primitives::block::BlockHash;
use zcash_protocol::consensus::BlockHeight;
use zecz::wallet::{check_address, AccountBirthday, AccountSpec};
use zecz::{Network, Profile, Wallet};

const TEST: Network = Network::TestNetwork;
const TEST_NU5: u32 = 1_842_420;

struct TempDir(PathBuf);

impl TempDir {
    fn new() -> Self {
        static N: AtomicU32 = AtomicU32::new(0);
        let p = std::env::temp_dir().join(format!("zecz-receive-test-{}-{}", std::process::id(), N.fetch_add(1, Ordering::SeqCst)));
        let _ = std::fs::remove_dir_all(&p);
        std::fs::create_dir_all(&p).unwrap();
        TempDir(p)
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn key_vector(name: &str) -> Value {
    let p = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../vectors/keys.json");
    let all: Vec<Value> = serde_json::from_slice(&std::fs::read(p).unwrap()).unwrap();
    all.into_iter().find(|v| v["name"] == name).expect("key vector")
}

#[test]
fn addresses_are_checked() {
    let kv = key_vector("test/abandon-art/pp0/acct0");
    assert_eq!(check_address(TEST, kv["ua"].as_str().unwrap()).unwrap(), "unified");
    assert_eq!(check_address(TEST, kv["ua_orchard_only"].as_str().unwrap()).unwrap(), "unified");
    assert_eq!(check_address(TEST, kv["transparent_address"].as_str().unwrap()).unwrap(), "transparent");
    let main = key_vector("main/abandon-art/pp0/acct0");
    let e = check_address(TEST, main["ua"].as_str().unwrap()).unwrap_err().to_string();
    assert!(e.contains("is not a valid test address"), "{e}");
    assert!(check_address(TEST, "utest1junk").is_err());
}

#[test]
fn next_address_rotates() {
    let d = TempDir::new();
    let kv = key_vector("test/abandon-art/pp0/acct0");
    let ufvk = UnifiedFullViewingKey::decode(&TEST, kv["ufvk"].as_str().unwrap()).unwrap();
    let birthday =
        AccountBirthday::from_parts(ChainState::empty(BlockHeight::from_u32(TEST_NU5 - 1), BlockHash([0; 32])), None);
    let spec = AccountSpec::view_only(kv["ufvk"].as_str().unwrap().into(), "watch");
    let (mut w, _) = Wallet::init_with_birthday(Profile::new(TEST, d.0.join("w"), None), &spec, &birthday).unwrap();
    let default = w.addresses().unwrap();

    let mut seen = vec![default.unified.clone()];
    let mut orchard_only = 0;
    for _ in 0..14 {
        let a = w.next_address().unwrap();
        assert_ne!(a.diversifier_index, 0);
        assert!(!seen.contains(&a.unified), "address repeated: {a:?}");
        seen.push(a.unified.clone());
        // Each is the account's own address at its index.
        let j = zip32::DiversifierIndex::try_from(a.diversifier_index).unwrap();
        let req = if a.transparent.is_some() {
            UnifiedAddressRequest::custom(Require, Omit, Require).unwrap()
        } else {
            orchard_only += 1;
            UnifiedAddressRequest::ORCHARD
        };
        assert_eq!(ufvk.address(j, req).unwrap().encode(&TEST), a.unified);
    }
    // The transparent gap limit (10 unused external addresses) is reached
    // within 14 rotations; past it the addresses are Orchard-only.
    assert!(orchard_only > 0, "never fell back to Orchard-only");
    // The default address is unchanged.
    assert_eq!(w.addresses().unwrap(), default);
}
