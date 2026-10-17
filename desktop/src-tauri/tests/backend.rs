//! The desktop backend end to end, without a window: profile status,
//! a watch-only import of the public test account (abandon x23 art,
//! vectors/keys.json), a sync with progress events, and receive
//! addresses with their QR code.
//!
//! The testnet test is #[ignore]d; run it with
//! `cargo test --test backend -- --ignored`.

use std::path::PathBuf;
use std::sync::Mutex;

use leekwallet_zcash_lib::backend::{self, SyncProgress};
use serde_json::Value;
use zecz::wallet::{AccountBirthday, AccountSpec};
use zecz::{Network, Profile, Wallet};

fn key_vector(name: &str) -> Value {
    let p = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../vectors/keys.json");
    let all: Vec<Value> = serde_json::from_slice(&std::fs::read(p).unwrap()).unwrap();
    all.into_iter().find(|v| v["name"] == name).expect("key vector")
}

/// A fresh directory under the system temp dir, removed on drop.
struct TempDir(PathBuf);

impl TempDir {
    fn new(tag: &str) -> Self {
        let p = std::env::temp_dir().join(format!("leekz-desktop-{tag}-{}", std::process::id()));
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

#[test]
fn empty_profile_and_bad_input() {
    let d = TempDir::new("offline");
    let profile = Profile::new(Network::TestNetwork, d.0.join("w"), None);
    let s = backend::profile_status(&profile).unwrap();
    assert_eq!((s.network.as_str(), s.has_account, s.birthday), ("test", false, None));
    assert_eq!(s.server, "https://testnet.zec.rocks:443");

    // Refused before any network or disk access.
    let e = backend::init_watch_only(&profile, "uviewtest1junk", 3_000_000).unwrap_err();
    assert!(format!("{e:#}").contains("bad UFVK"), "{e:#}");
    let ufvk = key_vector("test/abandon-art/pp0/acct0")["ufvk"].as_str().unwrap().to_string();
    let e = backend::init_watch_only(&profile, &ufvk, 1000).unwrap_err();
    assert!(format!("{e:#}").contains("before Sapling"), "{e:#}");
    assert!(!profile.exists());

    // The JSON the frontend reads.
    let p = SyncProgress::Scanned { start: 10, end: 20, tip: 30, fraction: 0.5 };
    assert_eq!(
        serde_json::to_value(&p).unwrap(),
        serde_json::json!({"kind": "scanned", "start": 10, "end": 20, "tip": 30, "fraction": 0.5})
    );
}

const TEST: Network = Network::TestNetwork;

/// A wallet of `spec` born at NU5 with empty trees: no lightwalletd.
fn offline_wallet(profile: &Profile, spec: &AccountSpec) {
    use zcash_client_backend::data_api::chain::ChainState;
    use zcash_primitives::block::BlockHash;
    use zcash_protocol::consensus::BlockHeight;
    let birthday = AccountBirthday::from_parts(ChainState::empty(BlockHeight::from_u32(1_842_419), BlockHash([0; 32])), None);
    Wallet::init_with_birthday(profile.clone(), spec, &birthday).unwrap();
}

#[test]
fn receive_addresses_and_qr() {
    let d = TempDir::new("receive");
    let profile = Profile::new(TEST, d.0.join("w"), None);
    let kv = key_vector("test/abandon-art/pp0/acct0");
    offline_wallet(&profile, &AccountSpec::view_only(kv["ufvk"].as_str().unwrap().into(), "watch"));
    let a = backend::next_address(&profile).unwrap();
    let b = backend::next_address(&profile).unwrap();
    assert_ne!(a.unified, b.unified);
    assert_ne!(a.unified, kv["ua"].as_str().unwrap());
    assert!(a.transparent.is_some());
    assert_ne!(a.diversifier_index, "0");
    // The default address stays what the device reports.
    assert_eq!(backend::addresses(&profile).unwrap().unified, kv["ua"].as_str().unwrap());

    let svg = backend::qr_svg(&a.unified).unwrap();
    assert!(svg.contains("<svg") && svg.contains("#000000") && svg.contains("#ffffff"), "{svg:.200}");
    assert!(backend::qr_svg(&"x".repeat(5000)).is_err());
}

#[test]
#[ignore = "needs testnet lightwalletd"]
fn watch_only_import_and_sync_on_testnet() {
    let d = TempDir::new("testnet");
    let profile = Profile::new(Network::TestNetwork, d.0.join("w"), None);
    let kv = key_vector("test/abandon-art/pp0/acct0");
    let ufvk = kv["ufvk"].as_str().unwrap();

    let tip = backend::tip(&profile.server).unwrap();
    assert_eq!(tip.chain_name, "test");
    // A few batches back, so progress is reported more than once.
    let birthday = tip.height as u32 - 3 * backend::SYNC_BATCH + 500;

    let imported = backend::init_watch_only(&profile, ufvk, birthday).unwrap();
    assert!(!imported.already_imported);
    assert_eq!(imported.birthday, birthday);
    assert_eq!(imported.addresses.unified, kv["ua"].as_str().unwrap());
    // Importing again is a no-op.
    assert!(backend::init_watch_only(&profile, ufvk, birthday).unwrap().already_imported);

    let s = backend::profile_status(&profile).unwrap();
    assert!(s.has_account);
    assert_eq!(s.birthday, Some(birthday));
    assert_eq!(s.fully_scanned, None);
    assert!(backend::balances(&profile).unwrap().is_none());

    let events = Mutex::new(Vec::new());
    let done = backend::sync(&profile, |e| events.lock().unwrap().push(e)).unwrap();
    let events = events.into_inner().unwrap();
    assert!(matches!(events.first(), Some(SyncProgress::Started { from, .. }) if *from == birthday));
    let fractions: Vec<f64> = events
        .iter()
        .filter_map(|e| match e {
            SyncProgress::Scanned { fraction, .. } => Some(*fraction),
            _ => None,
        })
        .collect();
    assert!(fractions.len() >= 2, "too little progress reported: {fractions:?}");
    assert!(fractions.windows(2).all(|w| w[0] <= w[1]), "progress went backwards: {fractions:?}");
    assert!(fractions.iter().all(|f| (0.0..=1.0).contains(f)));
    assert!(done.fully_scanned >= birthday);
    assert!(done.chain_tip >= tip.height as u32);
    println!("synced from {birthday}: progress {fractions:?}, done {done:?}");

    let b = backend::balances(&profile).unwrap().expect("balances after sync");
    assert_eq!(b.chain_tip, done.chain_tip);
    assert_eq!(b.total, b.orchard.total + b.ironwood.total + b.transparent.total);
    println!("balances {b:?}");

    let a = backend::addresses(&profile).unwrap();
    assert_eq!(a.unified, imported.addresses.unified);
    assert_eq!(a.transparent.as_deref(), kv["transparent_address"].as_str());

    let s = backend::profile_status(&profile).unwrap();
    assert_eq!(s.chain_tip, Some(done.chain_tip));
}
