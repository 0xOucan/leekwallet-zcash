//! The library API behind `zecz`, without a network: wallet profiles in a
//! temporary directory, importing the public test account (abandon x23
//! art) from a birthday built offline, address derivation against
//! vectors/keys.json, the refusals of `send` and `finish`, and `.zkv`
//! cards. The tests that talk to lightwalletd are #[ignore]d; run them with
//! `cargo test -p zecz-core -- --ignored`.

use std::path::PathBuf;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Mutex;

use serde_json::Value;
use zcash_primitives::block::BlockHash;
use zcash_client_backend::data_api::chain::ChainState;
use zcash_protocol::consensus::BlockHeight;
use zecz::keys::DevAccount;
use zecz::seed_card::{self, NewCard};
use zecz::wallet::{
    self, format_zec, parse_zec, AccountBirthday, AccountSpec, InitOutcome, SendRequest, SyncEvent,
};
use zecz::{Network, Profile, Wallet};

const ABANDON_ART: &str = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                           abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                           abandon art";

const TEST: Network = Network::TestNetwork;

/// A fresh, empty directory under the system temp dir, removed on drop.
struct TempDir(PathBuf);

impl TempDir {
    fn new() -> Self {
        static N: AtomicU32 = AtomicU32::new(0);
        let p = std::env::temp_dir().join(format!("zecz-core-test-{}-{}", std::process::id(), N.fetch_add(1, Ordering::SeqCst)));
        let _ = std::fs::remove_dir_all(&p);
        std::fs::create_dir_all(&p).unwrap();
        TempDir(p)
    }

    fn join(&self, p: &str) -> PathBuf {
        self.0.join(p)
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

fn dev_spec(account: u32) -> AccountSpec {
    let acct = DevAccount::from_mnemonic(&TEST, ABANDON_ART, "", account).unwrap();
    AccountSpec::from_dev_account(TEST, &acct, "leekwallet-dev").unwrap()
}

/// A birthday at NU5 activation with empty trees (not the real ones; fine
/// for a wallet that is never scanned): needs no lightwalletd.
fn offline_birthday() -> AccountBirthday {
    AccountBirthday::from_parts(ChainState::empty(BlockHeight::from_u32(TEST_NU5 - 1), BlockHash([0; 32])), None)
}

fn err_text<T>(r: anyhow::Result<T>) -> String {
    match r {
        Ok(_) => panic!("expected an error"),
        Err(e) => format!("{e:#}"),
    }
}

#[test]
fn profile_defaults_and_paths() {
    let p = Profile::new(TEST, "/tmp/w", None);
    assert_eq!(p.server, "https://testnet.zec.rocks:443");
    assert_eq!(p.net_name(), "test");
    assert_eq!(p.coin_type(), 1);
    assert_eq!(p.db_path(), PathBuf::from("/tmp/w/wallet.sqlite"));
    assert_eq!(p.pending_dir(), PathBuf::from("/tmp/w/pending"));
    assert_eq!(p.sent_dir(), PathBuf::from("/tmp/w/sent"));

    let m = Profile::new(Network::MainNetwork, "/tmp/w", Some("http://localhost:9067".into()));
    assert_eq!(m.server, "http://localhost:9067");
    assert_eq!(m.net_name(), "main");
    assert_eq!(m.coin_type(), 133);
    assert_eq!(wallet::default_server(Network::MainNetwork), "https://zec.rocks:443");
}

#[test]
fn open_needs_an_existing_wallet() {
    let d = TempDir::new();
    let p = Profile::new(TEST, d.join("w"), None);
    assert!(!p.exists());
    assert!(err_text(Wallet::open(p)).contains("no wallet in"));
}

#[test]
fn a_data_dir_is_bound_to_one_network() {
    let d = TempDir::new();
    let w = Wallet::create(Profile::new(TEST, d.join("w"), None)).unwrap();
    assert!(!w.has_account().unwrap());
    assert!(w.profile().exists());
    drop(w);
    assert_eq!(std::fs::read_to_string(d.join("w/network")).unwrap(), "test");
    let e = err_text(Wallet::open(Profile::new(Network::MainNetwork, d.join("w"), None)));
    assert!(e.contains("holds a test wallet"), "{e}");
    let e = err_text(Wallet::create(Profile::new(Network::MainNetwork, d.join("w"), None)));
    assert!(e.contains("holds a test wallet"), "{e}");
    // An empty wallet has no account to report on.
    let w = Wallet::open(Profile::new(TEST, d.join("w"), None)).unwrap();
    assert!(err_text(w.addresses()).contains("no account"));
}

#[test]
fn init_dev_imports_the_test_account_offline() {
    let d = TempDir::new();
    let kv = key_vector("test/abandon-art/pp0/acct0");
    let spec = dev_spec(0);
    assert_eq!(spec.ufvk, kv["ufvk"].as_str().unwrap());
    assert_eq!(hex_of(&spec.seed_fingerprint), kv["seed_fingerprint"].as_str().unwrap());

    let profile = Profile::new(TEST, d.join("w"), None);
    let (w, outcome) = Wallet::init_with_birthday(profile.clone(), &spec, &offline_birthday()).unwrap();
    let InitOutcome::Imported(a) = outcome else { panic!("expected a fresh import") };
    assert_eq!(a.name, "leekwallet-dev");
    assert_eq!(a.account_index, 0);
    assert_eq!(a.birthday, u32::from(TEST_NU5));
    assert_eq!(a.short_fingerprint(), "E62855DC");
    assert_eq!(a.addresses.unified, kv["ua"].as_str().unwrap());
    assert_eq!(a.addresses.orchard_only, kv["ua_orchard_only"].as_str().unwrap());
    assert_eq!(a.addresses.transparent.as_deref(), kv["transparent_address"].as_str());
    assert_eq!(w.ufvk().unwrap(), spec.ufvk);
    drop(w);

    // Reopened, it reports the same account.
    let w = Wallet::open(profile.clone()).unwrap();
    assert!(w.has_account().unwrap());
    assert_eq!(w.addresses().unwrap(), a.addresses);
    // Not scanned yet: no summary, no balance.
    assert_eq!(w.sync_status().unwrap(), None);
    assert!(err_text(w.balances()).contains("run `zecz sync`"));
    drop(w);

    // The same UFVK again is a no-op; another account is refused.
    let (_, again) = Wallet::init_with_birthday(profile.clone(), &spec, &offline_birthday()).unwrap();
    assert!(matches!(again, InitOutcome::AlreadyImported { addresses } if addresses == a.addresses));
    let e = err_text(Wallet::init_with_birthday(profile, &dev_spec(1), &offline_birthday()));
    assert!(e.contains("already tracks a different account"), "{e}");
}

const TEST_NU5: u32 = 1_842_420;

fn hex_of(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

#[test]
fn ufvk_shape_is_checked() {
    let e = err_text(wallet::parse_ufvk(TEST, "uview1junk"));
    assert!(e.contains("bad UFVK"), "{e}");
    // A mainnet UFVK is not a testnet one.
    let main = key_vector("main/abandon-art/pp0/acct0");
    assert!(wallet::parse_ufvk(TEST, main["ufvk"].as_str().unwrap()).is_err());
    let addrs = wallet::addresses_for_ufvk(Network::MainNetwork, main["ufvk"].as_str().unwrap()).unwrap();
    assert_eq!(addrs.unified, main["ua"].as_str().unwrap());
    assert_eq!(addrs.transparent.as_deref(), main["transparent_address"].as_str());
    assert_eq!(addrs.diversifier_index, 0);
}

#[test]
fn send_refusals() {
    let d = TempDir::new();
    let (mut w, _) =
        Wallet::init_with_birthday(Profile::new(TEST, d.join("w"), None), &dev_spec(0), &offline_birthday()).unwrap();
    let to = key_vector("test/abandon-art/pp0/acct1")["ua"].as_str().unwrap().to_string();
    let req = |to: &str, amount_zat| SendRequest { to: to.into(), amount_zat, memo: None, compact: true };

    let e = err_text(w.propose_send(&req("tmnotanaddress", 1000)));
    assert!(e.contains("is not a valid test address"), "{e}");
    let main_ua = key_vector("main/abandon-art/pp0/acct1")["ua"].as_str().unwrap().to_string();
    assert!(err_text(w.propose_send(&req(&main_ua, 1000))).contains("is not a valid test address"));
    assert!(err_text(w.propose_send(&req(&to, 0))).contains("amount must be positive"));
    // Never scanned: no target height to build for.
    assert!(err_text(w.propose_send(&req(&to, 1000))).contains("wallet is not synced"));
    assert!(!w.profile().pending_dir().exists());
}

#[tokio::test]
async fn finish_refuses_an_unknown_pczt() {
    let d = TempDir::new();
    let (mut w, _) =
        Wallet::init_with_birthday(Profile::new(TEST, d.join("w"), None), &dev_spec(0), &offline_birthday()).unwrap();
    let vectors = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../vectors");
    let unsigned = std::fs::read(vectors.join("pczt/iw_send_shielded.pczt")).unwrap();
    let acct = DevAccount::from_mnemonic(&TEST, ABANDON_ART, "", 0).unwrap();
    let zsig = zecz::soft_signer::sign(&unsigned, &acct, 1).unwrap().to_bytes().unwrap();

    // Signatures for another PCZT are refused before anything else.
    let other = std::fs::read(vectors.join("pczt/orchard_to_ironwood.pczt")).unwrap();
    assert!(w.finish(&other, &zsig, false, |_| {}).await.is_err());
    // Not built by this wallet: no pending full PCZT to prove.
    let e = err_text(w.finish(&unsigned, &zsig, false, |_| {}).await);
    assert!(e.contains("no pending PCZT"), "{e}");
}

#[test]
fn zec_amounts() {
    assert_eq!(parse_zec("1").unwrap(), 100_000_000);
    assert_eq!(parse_zec("0.001").unwrap(), 100_000);
    assert_eq!(parse_zec(".5").unwrap(), 50_000_000);
    assert_eq!(parse_zec("1.00000001").unwrap(), 100_000_001);
    assert!(parse_zec("1.000000001").is_err());
    assert!(parse_zec(".").is_err());
    assert!(parse_zec("abc").is_err());
    assert!(parse_zec("999999999999").is_err());
    assert_eq!(format_zec(123_456_789), "1.23456789 ZEC (123456789 zat)");
    assert_eq!(format_zec(0), "0.00000000 ZEC (0 zat)");
}

#[test]
fn seed_card_round_trip() {
    let d = TempDir::new();
    let out = d.join("SEED1.ZKV");
    let card = NewCard {
        mnemonic: ABANDON_ART,
        passphrase: "correct-horse-9",
        birthday: 3_000_000,
        account: 0,
        label: "test",
        iterations: zecz::zkv::ITERATIONS_MIN,
    };
    let size = seed_card::write_new(&card, &out).unwrap();
    assert_eq!(size as u64, std::fs::metadata(&out).unwrap().len());
    assert!(err_text(seed_card::write_new(&card, &out)).contains("exists"));
    assert!(seed_card::check_mnemonic("abandon art").is_err());

    let file = std::fs::read(&out).unwrap();
    let info = seed_card::describe(&file, "correct-horse-9", "", TEST).unwrap();
    assert_eq!(info.words, 24);
    assert_eq!(info.iterations, zecz::zkv::ITERATIONS_MIN);
    assert_eq!((info.label.as_str(), info.account, info.birthday, info.flags), ("test", 0, 3_000_000, 0));
    assert_eq!(info.short_fingerprint, "E62855DC");
    assert_eq!(info.ufvk, key_vector("test/abandon-art/pp0/acct0")["ufvk"].as_str().unwrap());
    assert!(seed_card::describe(&file, "wrong-pass-99", "", TEST).is_err());
}

// --- network (testnet lightwalletd) ---

#[tokio::test]
#[ignore = "needs testnet lightwalletd"]
async fn tip_from_testnet() {
    let t = wallet::tip(wallet::default_server(TEST)).await.unwrap();
    assert_eq!(t.chain_name, "test");
    assert!(t.height > u64::from(TEST_NU5));
}

#[tokio::test]
#[ignore = "needs testnet lightwalletd"]
async fn init_and_sync_near_the_tip() {
    let d = TempDir::new();
    let profile = Profile::new(TEST, d.join("w"), None);
    let tip = wallet::tip(&profile.server).await.unwrap().height as u32;
    let (mut w, outcome) = Wallet::init(profile, &dev_spec(0), tip - 20, |_| {}).await.unwrap();
    assert!(matches!(outcome, InitOutcome::Imported(_)));

    let events = Mutex::new(Vec::new());
    let status = w.sync(100, |e| events.lock().unwrap().push(e)).await.unwrap().expect("a summary after sync");
    let events = events.into_inner().unwrap();
    assert!(matches!(events.first(), Some(SyncEvent::Started { .. })));
    assert!(events.iter().any(|e| matches!(e, SyncEvent::Scanned(_))));
    assert!(status.fully_scanned >= tip - 20);

    let b = w.balances().unwrap();
    assert_eq!(b.chain_tip, status.chain_tip);
    assert!(b.account.is_some() || !b.synced);
}
