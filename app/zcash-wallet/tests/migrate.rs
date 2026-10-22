//! The move from the old desktop app (src/migrate.rs), in temporary
//! directories standing in for both apps' data: offline wallets of the
//! public test seed (abandon x23 art, vectors/keys.json) laid out as the
//! desktop app kept them, copied into this app's layout. The old directory
//! must come out byte for byte as it went in.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use leek_zcash_wallet::keys::{self, WalletKey};
use leek_zcash_wallet::{contacts, migrate, settings};
use serde_json::Value;
use zecz::wallet::{AccountBirthday, AccountSpec};
use zecz::{Network, Profile, Wallet};

const MAIN: Network = Network::MainNetwork;
const TEST: Network = Network::TestNetwork;
const ABANDON_ART: &str = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                           abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon \
                           abandon art";

fn key_vector(name: &str) -> Value {
    let p = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../vectors/keys.json");
    let all: Vec<Value> = serde_json::from_slice(&std::fs::read(p).unwrap()).unwrap();
    all.into_iter().find(|v| v["name"] == name).expect("key vector")
}

struct TempDir(PathBuf);

impl TempDir {
    fn new(tag: &str) -> Self {
        let p = std::env::temp_dir().join(format!("leekz-migrate-{tag}-{}", std::process::id()));
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

/// A wallet of `spec` born at `height`, with empty trees: no lightwalletd.
fn offline_wallet(dir: &Path, net: Network, spec: &AccountSpec, height: u32) {
    use zcash_client_backend::data_api::chain::ChainState;
    use zcash_primitives::block::BlockHash;
    use zcash_protocol::consensus::BlockHeight;
    // A chain state at h - 1 makes an account born at h.
    let birthday = AccountBirthday::from_parts(ChainState::empty(BlockHeight::from_u32(height - 1), BlockHash([0; 32])), None);
    Wallet::init_with_birthday(Profile::new(net, dir.to_path_buf(), None), spec, &birthday).unwrap();
}

/// The board account 0 of the test seed on `net`.
fn board_spec(net: Network) -> AccountSpec {
    let acct = zecz::keys::DevAccount::from_mnemonic(&net, ABANDON_ART, "", 0).unwrap();
    AccountSpec::from_dev_account(net, &acct, "leekwallet").unwrap()
}

fn board_key(net: Network) -> WalletKey {
    let k = key_vector(if net == MAIN { "main/abandon-art/pp0/acct0" } else { "test/abandon-art/pp0/acct0" });
    keys::WalletKey::parse(net, k["seed_fingerprint"].as_str().unwrap(), 0).unwrap()
}

/// Every file under `dir` with its bytes.
fn snapshot(dir: &Path) -> BTreeMap<PathBuf, Vec<u8>> {
    let mut out = BTreeMap::new();
    let mut stack = vec![dir.to_path_buf()];
    while let Some(d) = stack.pop() {
        for e in std::fs::read_dir(&d).unwrap() {
            let p = e.unwrap().path();
            if p.is_dir() {
                stack.push(p);
            } else {
                out.insert(p.strip_prefix(dir).unwrap().to_path_buf(), std::fs::read(&p).unwrap());
            }
        }
    }
    out
}

/// The old desktop app's data: a testnet board wallet with its address
/// book, a mainnet watch-only wallet with its book and swaps, settings.
fn old_desktop(old: &Path) {
    offline_wallet(&keys::legacy_wallet_dir(old, TEST), TEST, &board_spec(TEST), 3_000_000);
    let m = key_vector("main/abandon-art/pp0/acct0");
    offline_wallet(&keys::legacy_wallet_dir(old, MAIN), MAIN, &AccountSpec::view_only(m["ufvk"].as_str().unwrap().into(), "watch-only"), 3_500_000);
    let zoo = key_vector("test/zoo-vote/pp0/acct0");
    contacts::save(&contacts::path_in(old, TEST), TEST, "Zoo", zoo["ua"].as_str().unwrap(), None).unwrap();
    let t = key_vector("main/abandon-art/pp0/acct1");
    contacts::save(&contacts::path_in(old, MAIN), MAIN, "Exchange", t["transparent_address"].as_str().unwrap(), None).unwrap();
    std::fs::write(keys::legacy_swaps_path(old, MAIN), br#"{"version": 1, "swaps": []}"#).unwrap();
    let mut s = settings::Settings::default();
    s.set_server(MAIN, Some("https://zec.example.net:443")).unwrap();
    s.one_click_api_key = Some("partner.jwt".into());
    settings::save(&settings::path_in(old), &s).unwrap();
}

#[test]
fn the_old_apps_wallets_books_swaps_and_settings_are_copied_once() {
    let d = TempDir::new("copy");
    let (old, root) = (d.0.join("com.leekwallet.zcash"), d.0.join("io.leekwallet.zcash.companion/zcash"));
    old_desktop(&old);
    let before = snapshot(&old);

    let r = migrate::from_desktop(&old, &root).unwrap().expect("a report");
    assert_eq!(r.copied.len(), 6, "{r:#?}");
    assert!(r.skipped.is_empty(), "{r:#?}");

    // The testnet board wallet, at its account's place, with its book.
    let tk = board_key(TEST);
    let tp = Profile::new(TEST, tk.wallet_dir(&root), None);
    assert_eq!(keys::key_of_wallet(&tp).unwrap(), Some(tk.clone()));
    assert_eq!(contacts::list(&tk.contacts_path(&root)).unwrap()[0].name, "Zoo");
    // The mainnet watch-only wallet, named by its viewing key, with its
    // book and swaps.
    let m = key_vector("main/abandon-art/pp0/acct0");
    let mk = WalletKey::watch(MAIN, m["ufvk"].as_str().unwrap());
    assert_eq!(keys::key_of_wallet(&Profile::new(MAIN, mk.wallet_dir(&root), None)).unwrap(), Some(mk.clone()));
    assert_eq!(contacts::list(&mk.contacts_path(&root)).unwrap()[0].name, "Exchange");
    assert!(mk.swaps_path(&root).is_file());
    // The settings: the server and the key that were set.
    let s = settings::load(&settings::path_in(&root)).unwrap();
    assert_eq!(s.server_for(MAIN), "https://zec.example.net:443");
    assert_eq!(s.one_click_api_key.as_deref(), Some("partner.jwt"));
    // Each network keeps to itself, and the wallet list sees both.
    let tl = keys::list_wallets(&root, TEST).unwrap();
    assert_eq!(tl.len(), 1);
    assert!(!tl[0].watch_only && tl[0].wallet == tk.wallet() && tl[0].birthday == Some(3_000_000));
    let ml = keys::list_wallets(&root, MAIN).unwrap();
    assert_eq!(ml.len(), 1);
    assert!(ml[0].watch_only && ml[0].wallet == mk.wallet() && ml[0].zfp.is_none());
    // No staging left, the marker written, the old app untouched.
    assert!(!root.join("migrating").exists());
    assert!(root.join(migrate::MARKER).is_file());
    assert_eq!(snapshot(&old), before, "the old directory changed");

    // Once only: a second start does nothing, even with the copy gone.
    std::fs::remove_dir_all(tk.wallet_dir(&root)).unwrap();
    assert_eq!(migrate::from_desktop(&old, &root).unwrap(), None);
    assert!(!tk.wallet_dir(&root).exists());
}

#[test]
fn what_this_app_already_has_wins() {
    let d = TempDir::new("keep");
    let (old, root) = (d.0.join("old"), d.0.join("new"));
    old_desktop(&old);
    let before = snapshot(&old);

    // This app already has the testnet board wallet (the Companion made
    // it) and its address book; mainnet nothing.
    let tk = board_key(TEST);
    offline_wallet(&tk.wallet_dir(&root), TEST, &board_spec(TEST), 3_100_000);
    std::fs::write(tk.wallet_dir(&root).join("ours"), b"x").unwrap();
    let alice = key_vector("test/abandon-art/pp0/acct1");
    contacts::save(&tk.contacts_path(&root), TEST, "Alice", alice["ua"].as_str().unwrap(), None).unwrap();
    settings::save_server(&settings::path_in(&root), MAIN, Some("https://mine.example:443")).unwrap();

    let r = migrate::from_desktop(&old, &root).unwrap().unwrap();
    assert_eq!(r.skipped.len(), 2, "{r:#?}");
    // Ours, untouched: the wallet (its birthday, its marker) and the book.
    assert!(tk.wallet_dir(&root).join("ours").exists());
    assert_eq!(Wallet::open(Profile::new(TEST, tk.wallet_dir(&root), None)).unwrap().birthday().unwrap(), 3_100_000);
    let book = contacts::list(&tk.contacts_path(&root)).unwrap();
    assert_eq!(book.len(), 1);
    assert_eq!(book[0].name, "Alice");
    // Mainnet came over; our server stays, the key we did not have came.
    assert_eq!(keys::list_wallets(&root, MAIN).unwrap().len(), 1);
    let s = settings::load(&settings::path_in(&root)).unwrap();
    assert_eq!(s.server_for(MAIN), "https://mine.example:443");
    assert_eq!(s.one_click_api_key.as_deref(), Some("partner.jwt"));
    assert!(!root.join("migrating").exists());
    assert_eq!(snapshot(&old), before, "the old directory changed");
}

#[test]
fn nothing_to_copy_is_no_copy() {
    let d = TempDir::new("none");
    let root = d.0.join("new");
    // No old directory: nothing done, nothing recorded (it may yet appear).
    assert_eq!(migrate::from_desktop(&d.0.join("absent"), &root).unwrap(), None);
    assert!(!root.join(migrate::MARKER).exists());
    // An old directory without a wallet in it: recorded as done.
    let old = d.0.join("old");
    std::fs::create_dir_all(keys::legacy_wallet_dir(&old, TEST)).unwrap();
    let r = migrate::from_desktop(&old, &root).unwrap().unwrap();
    assert!(r.copied.is_empty());
    assert!(root.join(migrate::MARKER).is_file());
    assert!(keys::list_wallets(&root, TEST).unwrap().is_empty());
    // Beside this app's own data directory, under the old identifier.
    let here = Path::new("/home/u/.local/share/io.leekwallet.zcash.companion");
    assert_eq!(migrate::old_dir_beside(here).unwrap(), Path::new("/home/u/.local/share/com.leekwallet.zcash"));
}

#[test]
fn an_old_layout_wallet_settles_under_its_account() {
    let d = TempDir::new("settle");
    let root = &d.0;
    // The first Companion builds: one wallet per network, its book beside.
    offline_wallet(&keys::legacy_wallet_dir(root, TEST), TEST, &board_spec(TEST), 3_000_000);
    let zoo = key_vector("test/zoo-vote/pp0/acct0");
    contacts::save(&contacts::path_in(root, TEST), TEST, "Zoo", zoo["ua"].as_str().unwrap(), None).unwrap();
    let k = keys::settle_legacy(root, TEST).unwrap().expect("settled");
    assert_eq!(k, board_key(TEST));
    assert!(!keys::legacy_wallet_dir(root, TEST).exists());
    assert_eq!(contacts::list(&k.contacts_path(root)).unwrap().len(), 1);
    assert_eq!(keys::settle_legacy(root, TEST).unwrap(), None, "nothing left to settle");
}
