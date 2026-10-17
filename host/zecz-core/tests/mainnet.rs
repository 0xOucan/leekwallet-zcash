//! Mainnet (roadmap 10.3): the same wallet on coin type 133 with u1 / t1
//! addresses, offline against vectors/keys.json and the mainnet PCZT
//! vectors, plus the refusals that keep the two networks apart.
//!
//! The tests that talk to mainnet lightwalletd (zec.rocks) are #[ignore]d;
//! run them with `cargo test -p zecz-core --test mainnet -- --ignored`. They
//! only read the chain: a watch-only import of the public test account
//! (abandon x23 art), never a broadcast.

use std::path::PathBuf;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Mutex;

use pczt::Pczt;
use serde_json::Value;
use zcash_client_backend::data_api::chain::ChainState;
use zcash_primitives::block::BlockHash;
use zcash_protocol::consensus::BlockHeight;
use zecz::authorize::{apply_zsig, prove};
use zecz::keys::DevAccount;
use zecz::soft_signer;
use zecz::wallet::{self, AccountBirthday, AccountSpec, InitOutcome, SendRequest, SyncEvent};
use zecz::zsig::{Pool, Zsig};
use zecz::{Network, Profile, Wallet};

const ABANDON_ART: &str = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                           abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                           abandon art";

const MAIN: Network = Network::MainNetwork;
const TEST: Network = Network::TestNetwork;
/// Mainnet NU5 activation.
const MAIN_NU5: u32 = 1_687_104;

struct TempDir(PathBuf);

impl TempDir {
    fn new() -> Self {
        static N: AtomicU32 = AtomicU32::new(0);
        let p = std::env::temp_dir().join(format!("zecz-mainnet-test-{}-{}", std::process::id(), N.fetch_add(1, Ordering::SeqCst)));
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

fn vectors() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../vectors")
}

fn key_vector(name: &str) -> Value {
    let all: Vec<Value> = serde_json::from_slice(&std::fs::read(vectors().join("keys.json")).unwrap()).unwrap();
    all.into_iter().find(|v| v["name"] == name).expect("key vector")
}

fn pczt_vector(name: &str) -> (Value, Vec<u8>) {
    let all: Vec<Value> = serde_json::from_slice(&std::fs::read(vectors().join("pczt.json")).unwrap()).unwrap();
    let v = all.into_iter().find(|v| v["name"] == name).expect("vector");
    let bytes = std::fs::read(vectors().join(v["file"].as_str().unwrap())).unwrap();
    (v, bytes)
}

/// The public test account on mainnet, as a device account (its ZIP 32
/// origin recorded, so its PCZTs name the key a device would sign with).
fn main_spec() -> AccountSpec {
    let acct = DevAccount::from_mnemonic(&MAIN, ABANDON_ART, "", 0).unwrap();
    AccountSpec::from_dev_account(MAIN, &acct, "leekwallet").unwrap()
}

fn offline_birthday() -> AccountBirthday {
    AccountBirthday::from_parts(ChainState::empty(BlockHeight::from_u32(MAIN_NU5 - 1), BlockHash([0; 32])), None)
}

fn err_text<T>(r: anyhow::Result<T>) -> String {
    match r {
        Ok(_) => panic!("expected an error"),
        Err(e) => format!("{e:#}"),
    }
}

#[test]
fn mainnet_account_matches_the_vectors() {
    let v = key_vector("main/abandon-art/pp0/acct0");
    let spec = main_spec();
    assert_eq!(spec.ufvk, v["ufvk"].as_str().unwrap());
    assert!(spec.ufvk.starts_with("uview1"));

    let d = TempDir::new();
    let (w, outcome) = Wallet::init_with_birthday(Profile::new(MAIN, d.join("w"), None), &spec, &offline_birthday()).unwrap();
    let InitOutcome::Imported(a) = outcome else { panic!("not imported") };
    assert_eq!(a.addresses.unified, v["ua"].as_str().unwrap());
    assert!(a.addresses.unified.starts_with("u1"));
    assert_eq!(a.addresses.transparent.as_deref(), v["transparent_address"].as_str());
    assert!(a.addresses.transparent.as_deref().unwrap().starts_with("t1"));
    assert_eq!(a.addresses.orchard_only, v["ua_orchard_only"].as_str().unwrap());
    assert_eq!(std::fs::read_to_string(d.join("w/network")).unwrap(), "main");
    assert_eq!(w.profile().coin_type(), 133);
    assert_eq!(w.profile().server, "https://zec.rocks:443");
    assert_eq!(w.key_origin().unwrap().unwrap().short_fingerprint(), "E62855DC");

    // The testnet UFVK of the same seed is not a mainnet key.
    let t = key_vector("test/abandon-art/pp0/acct0");
    assert!(err_text(wallet::parse_ufvk(MAIN, t["ufvk"].as_str().unwrap())).contains("bad UFVK for this network"));
}

#[test]
fn mainnet_addresses_are_checked() {
    let m = key_vector("main/abandon-art/pp0/acct1");
    let t = key_vector("test/abandon-art/pp0/acct1");
    assert_eq!(wallet::check_address(MAIN, m["ua"].as_str().unwrap()).unwrap(), "unified");
    assert!(wallet::check_address(MAIN, m["transparent_address"].as_str().unwrap()).is_ok());
    for a in [&t["ua"], &t["transparent_address"]] {
        let e = err_text(wallet::check_address(MAIN, a.as_str().unwrap()));
        assert!(e.contains("is not a valid main address"), "{e}");
    }
    assert!(wallet::check_address(TEST, m["ua"].as_str().unwrap()).is_err());

    let d = TempDir::new();
    let (mut w, _) = Wallet::init_with_birthday(Profile::new(MAIN, d.join("w"), None), &main_spec(), &offline_birthday()).unwrap();
    let req = |to: &str| SendRequest { to: to.into(), amount_zat: 1000, memo: None, compact: true };
    let e = err_text(w.propose_send(&req(t["ua"].as_str().unwrap())));
    assert!(e.contains("is not a valid main address"), "{e}");
    assert!(err_text(w.propose_send(&req(m["ua"].as_str().unwrap()))).contains("wallet is not synced"));
}

#[test]
fn a_server_of_the_other_chain_is_refused() {
    assert!(wallet::check_chain(MAIN, "main").is_ok());
    assert!(wallet::check_chain(TEST, "test").is_ok());
    let e = err_text(wallet::check_chain(MAIN, "test"));
    assert!(e.contains("\"test\" chain") && e.contains("use a mainnet server"), "{e}");
    let e = err_text(wallet::check_chain(TEST, "main"));
    assert!(e.contains("use a testnet server"), "{e}");
    assert!(wallet::check_chain(MAIN, "regtest").is_err());
    assert!(wallet::check_chain(MAIN, "").is_err());
}

/// The mainnet PCZT vectors take the soft signer's coin type 133
/// signatures, prove and extract into transactions that verify; each is
/// refused under the testnet coin type.
#[test]
fn mainnet_vectors_sign_prove_and_extract() {
    use rand::{rand_core::UnwrapErr, rngs::SysRng};

    let acct = DevAccount::from_mnemonic(&MAIN, ABANDON_ART, "", 0).unwrap();
    for (name, n_t) in [("main_iw_send_shielded", 0usize), ("main_t_shield", 2)] {
        let (v, unsigned) = pczt_vector(name);
        assert_eq!(v["network"], "main");
        assert_eq!(*Pczt::parse(&unsigned).unwrap().global().coin_type(), 133, "{name}");
        assert!(soft_signer::sign(&unsigned, &acct, 1).is_err(), "{name}: signed as testnet");

        let zsig = soft_signer::sign(&unsigned, &acct, 133).unwrap();
        let parsed = Zsig::parse_for(&zsig.to_bytes().unwrap(), &unsigned).unwrap();
        assert_eq!(parsed.entries.iter().filter(|e| e.pool == Pool::Transparent).count(), n_t, "{name}");
        let signed = apply_zsig(Pczt::parse(&unsigned).unwrap(), &parsed).expect("signatures verify");
        let proven = prove(signed).unwrap();
        let finalized = pczt::roles::spend_finalizer::SpendFinalizer::new(proven).finalize_spends().unwrap();
        let tx = pczt::roles::tx_extractor::TransactionExtractor::new(finalized)
            .extract(UnwrapErr(SysRng))
            .expect("extraction verifies proofs and signatures");
        assert_eq!(u32::from(tx.consensus_branch_id()), 0x37a5_165b, "{name}");
    }

    // The cross-network vectors: a testnet signer refuses mainnet bytes, and
    // the reverse.
    let test_acct = DevAccount::from_mnemonic(&TEST, ABANDON_ART, "", 0).unwrap();
    for name in ["r_main_on_test", "r_main_shield_on_test"] {
        let (v, b) = pczt_vector(name);
        assert_eq!((v["network"].as_str(), v["expect"].as_str()), (Some("test"), Some("network_mismatch")));
        assert!(soft_signer::sign(&b, &test_acct, 1).is_err(), "{name}");
    }
    for name in ["r_test_on_main", "r_test_shield_on_main"] {
        let (v, b) = pczt_vector(name);
        assert_eq!((v["network"].as_str(), v["expect"].as_str()), (Some("main"), Some("network_mismatch")));
        assert!(soft_signer::sign(&b, &acct, 133).is_err(), "{name}");
    }
}

// --- network (mainnet lightwalletd; read only) ---

#[tokio::test]
#[ignore = "needs mainnet lightwalletd"]
async fn tip_from_mainnet() {
    let t = wallet::tip(wallet::default_server(MAIN)).await.unwrap();
    assert_eq!(t.chain_name, "main");
    assert!(t.height > 3_428_143, "below NU6.3: {}", t.height);
    println!("mainnet tip {} branch {}", t.height, t.consensus_branch_id);
}

/// A watch-only import of the public test account about 1,000 blocks below
/// the mainnet tip: sync, balances (zero), and the refusals of a send and a
/// shield with nothing to spend. Nothing is broadcast.
#[tokio::test]
#[ignore = "needs mainnet lightwalletd"]
async fn mainnet_watch_only_sync_near_the_tip() {
    let d = TempDir::new();
    let profile = Profile::new(MAIN, d.join("w"), None);
    let tip = wallet::tip(&profile.server).await.unwrap().height as u32;
    let birthday = tip - 1_000;
    let started = std::time::Instant::now();
    let (mut w, outcome) = Wallet::init(profile, &main_spec(), birthday, |_| {}).await.unwrap();
    assert!(matches!(outcome, InitOutcome::Imported(_)));

    let events = Mutex::new(Vec::new());
    let status = w.sync(1000, |e| events.lock().unwrap().push(e)).await.unwrap().expect("a summary after sync");
    let events = events.into_inner().unwrap();
    assert!(matches!(events.first(), Some(SyncEvent::Started { .. })));
    assert!(status.fully_scanned >= tip, "scanned to {} of {tip}", status.fully_scanned);
    println!("synced {} blocks to {} in {:?}", tip - birthday, status.fully_scanned, started.elapsed());

    let b = w.balances().unwrap();
    assert!(b.synced);
    let a = b.account.expect("an account balance");
    // The public test account: nobody should pay it on mainnet. If someone
    // did, this says so rather than failing.
    if a.total != 0 {
        println!("NOTE: the public test account holds {} zat on mainnet", a.total);
    }
    let to = key_vector("main/abandon-art/pp0/acct1")["ua"].as_str().unwrap().to_string();
    if a.spendable == 0 {
        let e = err_text(w.propose_send(&SendRequest { to, amount_zat: 10_000, memo: None, compact: true }));
        assert!(e.contains("Insufficient balance"), "{e}");
    }
    if a.transparent.total == 0 {
        let e = err_text(w.propose_shield(true));
        assert!(e.contains("Insufficient funds") || e.contains("nothing to shield"), "{e}");
    }
    assert!(!w.profile().pending_dir().exists() || std::fs::read_dir(w.profile().pending_dir()).unwrap().next().is_none());
}

/// A mainnet wallet pointed at a testnet server is refused before it
/// stores anything from that chain.
#[tokio::test]
#[ignore = "needs testnet lightwalletd"]
async fn a_mainnet_wallet_refuses_a_testnet_server() {
    let d = TempDir::new();
    let (_, _) = Wallet::init_with_birthday(Profile::new(MAIN, d.join("w"), None), &main_spec(), &offline_birthday()).unwrap();
    let mut w = Wallet::open(Profile::new(MAIN, d.join("w"), Some(wallet::default_server(TEST).into()))).unwrap();
    let e = err_text(w.sync(100, |_| {}).await);
    assert!(e.contains("follows the \"test\" chain"), "{e}");
    assert!(w.sync_status().unwrap().is_none(), "something was stored from the testnet");

    let d2 = TempDir::new();
    let p = Profile::new(MAIN, d2.join("w"), Some(wallet::default_server(TEST).into()));
    let e = err_text(Wallet::init(p, &main_spec(), 3_500_000, |_| {}).await);
    assert!(e.contains("follows the \"test\" chain"), "{e}");
}
