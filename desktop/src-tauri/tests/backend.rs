//! The desktop backend end to end, without a window: profile status,
//! a watch-only import of the public test account (abandon x23 art,
//! vectors/keys.json), a sync with progress events, and the wallet
//! screens: history (a real transaction stored in an offline wallet),
//! receive addresses and their QR code, shield refusals, contacts.
//!
//! The testnet test is #[ignore]d; run it with
//! `cargo test --test backend -- --ignored`.

use std::path::PathBuf;
use std::sync::Mutex;

use leekwallet_zcash_lib::backend::{self, SyncProgress};
use leekwallet_zcash_lib::{contacts, hardware};
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
const ABANDON_ART: &str = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                           abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                           abandon art";

/// A wallet of `spec` born at NU5 with empty trees: no lightwalletd.
fn offline_wallet(profile: &Profile, spec: &AccountSpec) {
    use zcash_client_backend::data_api::chain::ChainState;
    use zcash_primitives::block::BlockHash;
    use zcash_protocol::consensus::BlockHeight;
    let birthday = AccountBirthday::from_parts(ChainState::empty(BlockHeight::from_u32(1_842_419), BlockHash([0; 32])), None);
    Wallet::init_with_birthday(profile.clone(), spec, &birthday).unwrap();
}

/// Vector `iw_send_shielded` (abandon-art pays 0.004 TAZ with a memo to
/// zoo-vote), soft-signed, proven and extracted: a real transaction to
/// put in an offline wallet.
fn iw_send_shielded_tx() -> Vec<u8> {
    use pczt::Pczt;
    use rand::{rand_core::UnwrapErr, rngs::SysRng};
    let unsigned =
        std::fs::read(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../vectors/pczt/iw_send_shielded.pczt")).unwrap();
    let acct = zecz::keys::DevAccount::from_mnemonic(&TEST, ABANDON_ART, "", 0).unwrap();
    let zsig = zecz::soft_signer::sign(&unsigned, &acct, 1).unwrap();
    let signed = zecz::authorize::apply_zsig(Pczt::parse(&unsigned).unwrap(), &zsig).unwrap();
    let proven = zecz::authorize::prove(signed).unwrap();
    let finalized = pczt::roles::spend_finalizer::SpendFinalizer::new(proven).finalize_spends().unwrap();
    let tx = pczt::roles::tx_extractor::TransactionExtractor::new(finalized).extract(UnwrapErr(SysRng)).unwrap();
    let mut raw = Vec::new();
    tx.write(&mut raw).unwrap();
    raw
}

#[test]
fn history_of_an_offline_wallet() {
    let d = TempDir::new("history");
    let profile = Profile::new(TEST, d.0.join("w"), None);
    // No wallet yet: an empty history, not an error.
    assert!(backend::history(&profile).unwrap().is_empty());

    let zoo = key_vector("test/zoo-vote/pp0/acct0");
    offline_wallet(&profile, &AccountSpec::view_only(zoo["ufvk"].as_str().unwrap().into(), "zoo"));
    assert!(backend::history(&profile).unwrap().is_empty());

    let mut w = Wallet::open(profile.clone()).unwrap();
    let txid = w.store_transaction(&iw_send_shielded_tx(), Some(4_200_000)).unwrap();
    drop(w);
    let h = backend::history(&profile).unwrap();
    assert_eq!(h.len(), 1);
    let t = &h[0];
    assert_eq!(t.txid, txid);
    assert_eq!((t.kind, t.amount_zat, t.delta_zat), ("received", 400_000, 400_000));
    assert_eq!(t.mined_height, Some(4_200_000));
    assert_eq!(t.memos, vec!["LeekWallet Zcash test".to_string()]);
    // The payer's fee is not this wallet's to show.
    assert_eq!(t.fee_zat, None);
    assert!(!t.expired);

    // The JSON the frontend reads.
    let v = serde_json::to_value(t).unwrap();
    assert_eq!(v["kind"], "received");
    assert_eq!(v["amountZat"], 400_000);
    assert_eq!(v["memos"][0], "LeekWallet Zcash test");
    assert!(v["blockTime"].is_null() && v["feeZat"].is_null());
    assert_eq!(v["txid"].as_str().unwrap().len(), 64);
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
fn shield_refusals() {
    let d = TempDir::new("shield");
    let kv = key_vector("test/abandon-art/pp0/acct0");

    // Watch-only: nothing could sign it.
    let watch = Profile::new(TEST, d.0.join("watch"), None);
    offline_wallet(&watch, &AccountSpec::view_only(kv["ufvk"].as_str().unwrap().into(), "watch"));
    let e = format!("{:#}", hardware::propose_shield(&watch).unwrap_err());
    assert!(e.contains("watch-only"), "{e}");

    // A device account never synced: no height to build for. (The fee
    // swallowing the whole balance is refused in zecz: host/zecz-core
    // tests/shield.rs.)
    let dev = Profile::new(TEST, d.0.join("dev"), None);
    let acct = zecz::keys::DevAccount::from_mnemonic(&TEST, ABANDON_ART, "", 0).unwrap();
    offline_wallet(&dev, &AccountSpec::from_dev_account(TEST, &acct, "leekwallet").unwrap());
    let e = format!("{:#}", hardware::propose_shield(&dev).unwrap_err());
    assert!(e.contains("not synced"), "{e}");
    assert!(!dev.pending_dir().exists());
}

#[test]
fn contacts_are_stored_as_json() {
    let d = TempDir::new("contacts");
    let path = contacts::path_in(&d.0.join("app"), TEST);
    assert!(path.ends_with("app/contacts-test.json"));
    assert!(contacts::list(&path).unwrap().is_empty());

    let zoo = key_vector("test/zoo-vote/pp0/acct0");
    let abandon = key_vector("test/abandon-art/pp0/acct1");
    let zoo_ua = zoo["ua"].as_str().unwrap();
    let t = abandon["transparent_address"].as_str().unwrap();

    contacts::save(&path, TEST, "  zoo  ", zoo_ua, None).unwrap();
    let book = contacts::save(&path, TEST, "Alice", t, None).unwrap();
    // Sorted by name, ignoring case; names trimmed.
    let names: Vec<&str> = book.iter().map(|c| c.name.as_str()).collect();
    assert_eq!(names, ["Alice", "zoo"]);
    assert_eq!(contacts::list(&path).unwrap(), book);

    // The file: versioned JSON, readable by hand.
    let json: Value = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
    assert_eq!(json["version"], 1);
    assert_eq!(json["contacts"][0]["name"], "Alice");
    assert_eq!(json["contacts"][0]["address"], t);

    // Refusals: names are unique ignoring case; addresses must be payable
    // on this network; empty and overlong names.
    let e = format!("{:#}", contacts::save(&path, TEST, "ALICE", zoo_ua, None).unwrap_err());
    assert!(e.contains("already a contact"), "{e}");
    let main_ua = key_vector("main/abandon-art/pp0/acct0")["ua"].as_str().unwrap().to_string();
    let e = format!("{:#}", contacts::save(&path, TEST, "Bob", &main_ua, None).unwrap_err());
    assert!(e.contains("not a valid test address"), "{e}");
    assert!(contacts::save(&path, TEST, "  ", zoo_ua, None).is_err());
    assert!(contacts::save(&path, TEST, &"n".repeat(contacts::MAX_NAME + 1), zoo_ua, None).is_err());
    assert_eq!(contacts::list(&path).unwrap().len(), 2);

    // Rename (and readdress) one; then remove one.
    let book = contacts::save(&path, TEST, "Zoo Vote", zoo["ua_orchard_only"].as_str().unwrap(), Some("zoo")).unwrap();
    assert_eq!(book.iter().map(|c| c.name.as_str()).collect::<Vec<_>>(), ["Alice", "Zoo Vote"]);
    assert_eq!(book[1].address, zoo["ua_orchard_only"].as_str().unwrap());
    assert!(contacts::save(&path, TEST, "X", zoo_ua, Some("nobody")).is_err());
    let book = contacts::remove(&path, "Alice").unwrap();
    assert_eq!(book.len(), 1);
    assert!(contacts::remove(&path, "Alice").is_err());
    assert_eq!(contacts::list(&path).unwrap(), book);
    // No temporary file left behind.
    assert_eq!(std::fs::read_dir(path.parent().unwrap()).unwrap().count(), 1);

    // A file this app cannot read is an error, not an empty book.
    std::fs::write(&path, b"{not json").unwrap();
    assert!(contacts::list(&path).is_err());
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

    // The history of what the sync found, newest first.
    let h = backend::history(&profile).unwrap();
    for t in &h {
        assert!(t.mined_height.map_or(true, |m| m >= birthday), "{t:?}");
        println!("{} {} {} zat at {:?} memos {:?}", t.txid, t.kind, t.amount_zat, t.mined_height, t.memos);
    }
    let heights: Vec<u32> = h.iter().filter_map(|t| t.mined_height).collect();
    assert!(heights.windows(2).all(|w| w[0] >= w[1]), "not newest first: {heights:?}");
}
