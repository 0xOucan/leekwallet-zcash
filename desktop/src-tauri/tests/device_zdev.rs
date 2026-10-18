//! Hardware accounts without a board: `zdev` (firmware/sim) runs the
//! firmware's protocol, session, signers and review pages on a PTY, holding
//! the public test seed (abandon x23 art), and answers its prompts from
//! stdin (`--yes` answers all of them yes). These tests drive the desktop
//! backend against it the way the app does: pair over "USB", import the
//! account, verify the address, and sign a PCZT; the signatures then prove
//! and extract into a transaction that verifies.
//!
//! A real send needs a funded, synced wallet; that one is #[ignore]d and
//! needs testnet lightwalletd. Bluetooth needs a radio and is not tested
//! here (src/link.rs only compiles).
//!
//! Builds zdev with `make` if it is missing; skips (loudly) if it cannot.

use std::io::Write;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use leekwallet_zcash_lib::backend;
use leekwallet_zcash_lib::hardware::{self, DeviceSession, DeviceStatus};
use leekwallet_zcash_lib::link;
use pczt::Pczt;
use serde_json::Value;
use zcash_client_backend::data_api::chain::ChainState;
use zcash_primitives::block::BlockHash;
use zcash_protocol::consensus::BlockHeight;
use zecz::device::{DeviceError, ERR_USER_REJECTED};
use zecz::wallet::{AccountBirthday, AccountSpec, InitOutcome};
use zecz::zsig::Zsig;
use zecz::{Network, Profile, Wallet};

const TEST: Network = Network::TestNetwork;

fn repo() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn key_vector(name: &str) -> Value {
    let all: Vec<Value> = serde_json::from_slice(&std::fs::read(repo().join("vectors/keys.json")).unwrap()).unwrap();
    all.into_iter().find(|v| v["name"] == name).expect("key vector")
}

fn pczt_vector(name: &str) -> (Value, Vec<u8>) {
    let v = repo().join("vectors");
    let all: Vec<Value> = serde_json::from_slice(&std::fs::read(v.join("pczt.json")).unwrap()).unwrap();
    let e = all.into_iter().find(|e| e["name"] == name).expect("vector");
    let bytes = std::fs::read(v.join(e["file"].as_str().unwrap())).unwrap();
    (e, bytes)
}

/// A fresh directory under the system temp dir, removed on drop.
struct TempDir(PathBuf);

impl TempDir {
    fn new(tag: &str) -> Self {
        let p = std::env::temp_dir().join(format!("leekz-desktop-dev-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&p);
        std::fs::create_dir_all(&p).unwrap();
        TempDir(p)
    }

    fn profile(&self, name: &str) -> Profile {
        Profile::new(TEST, self.0.join(name), None)
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

// --------------------------------------------------------------------- zdev

/// Builds zdev from the current firmware sources, once per test binary.
/// Always through make (incremental): reusing an existing binary only when
/// it is missing once ran these tests against a stale device.
fn zdev_binary() -> Option<PathBuf> {
    static BUILT: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    let sim = repo().join("firmware/sim");
    let ok = *BUILT.get_or_init(|| {
        Command::new("make").arg("-C").arg(&sim).arg("build/zdev").status().is_ok_and(|s| s.success())
    });
    ok.then(|| sim.join("build/zdev"))
}

/// A running zdev, killed on drop.
struct Zdev {
    child: Child,
    link: PathBuf,
    dir: PathBuf,
}

impl Drop for Zdev {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}

impl Zdev {
    fn port(&self) -> String {
        self.link.to_str().unwrap().to_string()
    }

    /// Queues answers for the device's prompts ("y\n" or "n\n" each).
    fn answer(&mut self, answers: &str) {
        self.child.stdin.as_mut().unwrap().write_all(answers.as_bytes()).unwrap();
    }
}

/// zdev on testnet (its Settings > Network), or on `net` with `start_on`.
fn start(tag: &str, auto_yes: bool) -> Option<Zdev> {
    start_on(tag, auto_yes, "test")
}

fn start_on(tag: &str, auto_yes: bool, net: &str) -> Option<Zdev> {
    let bin = zdev_binary()?;
    let dir = std::env::temp_dir().join(format!("leekz-desktop-zdev-{tag}-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let link = dir.join("port");
    let mut cmd = Command::new(bin);
    cmd.arg("--link").arg(&link).arg("--network").arg(net);
    cmd.stdin(Stdio::piped()).stdout(Stdio::null()).stderr(Stdio::null());
    if auto_yes {
        cmd.arg("--yes");
    }
    let child = cmd.spawn().ok()?;
    let deadline = Instant::now() + Duration::from_secs(10);
    while !link.exists() && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(50));
    }
    Some(Zdev { child, link, dir })
}

macro_rules! zdev_or_skip {
    ($tag:expr, $yes:expr) => {
        zdev_or_skip!($tag, $yes, "test")
    };
    ($tag:expr, $yes:expr, $net:expr) => {
        match start_on($tag, $yes, $net) {
            Some(z) => z,
            None => {
                eprintln!("SKIPPED: zdev could not be built (make -C firmware/sim build/zdev)");
                return;
            }
        }
    };
}

/// What a flow reported: the passkeys and the status steps.
#[derive(Default)]
struct Seen {
    passkeys: Mutex<Vec<String>>,
    status: Mutex<Vec<DeviceStatus>>,
}

impl Seen {
    fn passkey(&self) -> impl Fn(&str) + '_ {
        move |p| self.passkeys.lock().unwrap().push(p.to_string())
    }

    fn status(&self) -> impl Fn(DeviceStatus) + '_ {
        move |s| self.status.lock().unwrap().push(s)
    }

    fn steps(&self) -> Vec<&'static str> {
        let mut v: Vec<&'static str> = self.status.lock().unwrap().iter().map(|s| s.step).collect();
        v.dedup();
        v
    }

    fn clear(&self) {
        self.status.lock().unwrap().clear();
    }
}

/// Pairs with zdev over its PTY, as `device_pair` with transport "usb" does.
fn connect(z: &Zdev, profile: &Profile, seen: &Seen) -> DeviceSession {
    let port = z.port();
    let dev = link::open_usb(&port).expect("open zdev");
    hardware::connect(dev, "usb", &port, 0, profile, &seen.passkey(), &seen.status()).expect("pair")
}

/// `Wallet::init` with the tree state left empty, so no lightwalletd is
/// needed (fine for a wallet that is never scanned).
fn offline_init(p: &Profile, spec: &AccountSpec, birthday: u32) -> anyhow::Result<(Wallet, InitOutcome)> {
    let b = AccountBirthday::from_parts(ChainState::empty(BlockHeight::from_u32(birthday - 1), BlockHash([0; 32])), None);
    Wallet::init_with_birthday(p.clone(), spec, &b)
}

fn import_offline(profile: &Profile, s: &mut DeviceSession, birthday: Option<u32>, seen: &Seen) -> anyhow::Result<hardware::DeviceImported> {
    hardware::device_import_with(profile, s, birthday, &seen.status(), offline_init)
}

fn err_text<T>(r: anyhow::Result<T>) -> anyhow::Error {
    match r {
        Ok(_) => panic!("expected an error"),
        Err(e) => e,
    }
}

// -------------------------------------------------------------------- tests

#[test]
fn pair_import_verify_and_sign() {
    use rand::{rand_core::UnwrapErr, rngs::SysRng};

    let z = zdev_or_skip!("flow", true);
    let d = TempDir::new("flow");
    let profile = d.profile("w");
    let kv = key_vector("test/abandon-art/pp0/acct0");
    let seen = Seen::default();

    // Pair: one six-digit passkey for the user to compare, then the account.
    let mut s = connect(&z, &profile, &seen);
    let passkeys = seen.passkeys.lock().unwrap().clone();
    assert_eq!(passkeys.len(), 1);
    assert!(passkeys[0].len() == 6 && passkeys[0].bytes().all(|b| b.is_ascii_digit()), "{passkeys:?}");
    assert_eq!(seen.steps(), ["pair", "unlock", "account"]);
    assert_eq!(s.account.ua, kv["ua"].as_str().unwrap());
    assert_eq!(s.account.birthday, None, "a fresh zdev knows no birthday");
    let paired = s.describe(&profile).unwrap();
    assert_eq!((paired.transport.as_str(), paired.zfp.as_str()), ("usb", "E62855DC"));
    assert_eq!(paired.matches_wallet, None, "no wallet yet");

    // Import: a birthday is needed when the device has none.
    let e = err_text(import_offline(&profile, &mut s, None, &seen));
    assert!(e.to_string().contains("does not know this account's birthday"), "{e:#}");
    assert!(!profile.exists(), "nothing written before the birthday is known");
    let imported = import_offline(&profile, &mut s, Some(3_000_123), &seen).expect("import");
    assert!(!imported.already_imported);
    assert_eq!((imported.birthday, imported.birthday_from), (3_000_123, "user"));
    assert_eq!(imported.birthday_stored, Some(true), "zdev's stored seed keeps it in flash");
    assert_eq!(imported.zfp, "E62855DC");
    assert_eq!(imported.addresses.unified, kv["ua"].as_str().unwrap());
    let st = backend::profile_status(&profile).unwrap();
    assert!(st.has_account && st.hardware, "{st:?}");
    assert_eq!(st.zfp.as_deref(), Some("E62855DC"));
    assert_eq!(st.birthday, Some(3_000_123));
    assert_eq!(s.describe(&profile).unwrap().matches_wallet, Some(true));

    // Importing again changes nothing, and the device already has the birthday.
    let again = import_offline(&profile, &mut s, None, &seen).expect("again");
    assert!(again.already_imported);
    assert_eq!((again.birthday, again.birthday_from, again.birthday_stored), (3_000_123, "wallet", None));

    // Verify the address the receive card shows.
    seen.clear();
    let ua = backend::addresses(&profile).unwrap().unified;
    hardware::verify_address(&profile, &mut s, &ua, &seen.status()).expect("confirmed on the device");
    assert_eq!(seen.steps(), ["verify"]);
    // Someone else's address is refused by the device before its screen.
    let other = key_vector("test/abandon-art/pp0/acct1");
    let e = err_text(hardware::verify_address(&profile, &mut s, other["ua"].as_str().unwrap(), &seen.status()));
    assert!(hardware::explain(&e).contains("NOT from its wallet"), "{}", hardware::explain(&e));
    assert!(!s.lost, "a refusal is not a broken link");

    // Sign a PCZT (the compact view, as propose_send makes) on the device.
    seen.clear();
    let (_, view) = pczt_vector("iw_send_shielded_compact");
    let zsig = hardware::device_sign(&profile, &mut s, &view, &seen.status()).expect("device signs");
    assert_eq!(seen.steps(), ["upload", "review", "signed"]);
    let last_upload = seen.status.lock().unwrap().iter().rev().find(|x| x.step == "upload").cloned().unwrap();
    assert_eq!((last_upload.done, last_upload.total), (Some(view.len() as u64), Some(view.len() as u64)));

    // What finish does with it, minus the wallet's pending PCZT (the vector
    // is not this wallet's): the .zsig binds to the view, the signatures
    // verify against rk, and the proven transaction extracts. The compact
    // view's signatures apply to its full PCZT.
    let parsed = Zsig::parse_for(&zsig, &view).expect(".zsig for this view");
    assert_eq!(parsed.entries.len(), 1);
    let (_, full) = pczt_vector("iw_send_shielded");
    let full = Pczt::parse(&full).unwrap();
    let signed = zecz::authorize::apply_zsig(full, &parsed).expect("signatures verify against rk");
    let proven = zecz::authorize::prove(signed).unwrap();
    let finalized = pczt::roles::spend_finalizer::SpendFinalizer::new(proven).finalize_spends().unwrap();
    pczt::roles::tx_extractor::TransactionExtractor::new(finalized)
        .extract(UnwrapErr(SysRng))
        .expect("extraction verifies proofs and the device's signatures");

    // A second companion (a fresh wallet, a new pairing) takes the birthday
    // the first one recorded on the device.
    drop(s);
    let profile2 = d.profile("w2");
    let mut s2 = connect(&z, &profile2, &seen);
    assert_eq!(s2.account.birthday, Some(3_000_123));
    let imported2 = import_offline(&profile2, &mut s2, None, &seen).expect("import from the device's birthday");
    assert_eq!((imported2.birthday, imported2.birthday_from, imported2.birthday_stored), (3_000_123, "device", None));
}

#[test]
fn a_rejection_on_the_device_is_explained() {
    let mut z = zdev_or_skip!("reject", false);
    // Pairing: yes. Review: no.
    z.answer("y\nn\n");
    let d = TempDir::new("reject");
    let profile = d.profile("w");
    // The wallet's device account, imported as `zecz init` would.
    let kv = key_vector("test/abandon-art/pp0/acct0");
    let fp: [u8; 32] = hex::decode(kv["seed_fingerprint"].as_str().unwrap()).unwrap().try_into().unwrap();
    let spec = AccountSpec::device(kv["ufvk"].as_str().unwrap().to_string(), fp, 0, "leekwallet");
    offline_init(&profile, &spec, 3_000_000).unwrap();

    let seen = Seen::default();
    let mut s = connect(&z, &profile, &seen);
    assert_eq!(s.describe(&profile).unwrap().matches_wallet, Some(true));
    let (_, view) = pczt_vector("iw_send_shielded_compact");
    let e = err_text(hardware::device_sign(&profile, &mut s, &view, &seen.status()));
    assert!(zecz::device::is_code(&e, ERR_USER_REJECTED), "{e:#}");
    assert!(hardware::explain(&e).starts_with("Rejected on the device."), "{}", hardware::explain(&e));
    assert!(!s.lost, "the session survives a no");
}

#[test]
fn another_seed_is_refused_before_anything_is_sent() {
    let z = zdev_or_skip!("other-seed", true);
    let d = TempDir::new("other-seed");
    let profile = d.profile("w");
    // A wallet for a seed zdev does not hold.
    let kv = key_vector("test/zoo-vote/pp0/acct0");
    let fp: [u8; 32] = hex::decode(kv["seed_fingerprint"].as_str().unwrap()).unwrap().try_into().unwrap();
    let spec = AccountSpec::device(kv["ufvk"].as_str().unwrap().to_string(), fp, 0, "leekwallet");
    offline_init(&profile, &spec, 3_000_000).unwrap();

    let seen = Seen::default();
    let mut s = connect(&z, &profile, &seen);
    assert_eq!(s.describe(&profile).unwrap().matches_wallet, Some(false));
    seen.clear();
    let (_, view) = pczt_vector("iw_send_shielded_compact");
    let e = err_text(hardware::device_sign(&profile, &mut s, &view, &seen.status()));
    assert!(e.to_string().contains("holds another seed"), "{e:#}");
    let e = err_text(hardware::verify_address(&profile, &mut s, kv["ua"].as_str().unwrap(), &seen.status()));
    assert!(e.to_string().contains("holds another seed"), "{e:#}");
    assert!(seen.steps().is_empty(), "nothing reached the device: {:?}", seen.steps());

    // And importing over a wallet that tracks another account is refused.
    let e = err_text(import_offline(&profile, &mut s, Some(3_000_000), &seen));
    assert!(format!("{e:#}").contains("already tracks a different account"), "{e:#}");
}

/// Roadmap 10.3: the same flows on mainnet. The device is asked for "main"
/// and exports the coin type 133 account; it verifies the u1 address and
/// signs a mainnet PCZT, and refuses a testnet one (-17). A session paired
/// for mainnet is refused for a testnet wallet before anything reaches the
/// device (what a network switch must never allow), and a mainnet
/// sign_and_send needs the user's extra confirmation.
#[test]
fn mainnet_flows_and_a_session_bound_to_its_network() {
    let z = zdev_or_skip!("main", true, "main");
    let d = TempDir::new("main");
    let main = Profile::new(Network::MainNetwork, d.0.join("wallets/main"), None);
    let test = Profile::new(TEST, d.0.join("wallets/test"), None);
    let kv = key_vector("main/abandon-art/pp0/acct0");
    let seen = Seen::default();

    let mut s = connect(&z, &main, &seen);
    assert_eq!(s.network, "main");
    assert_eq!(s.account.ua, kv["ua"].as_str().unwrap());
    assert_eq!(s.account.t_address, kv["transparent_address"].as_str().unwrap());
    assert!(s.account.ua.starts_with("u1") && s.account.t_address.starts_with("t1"));
    let imported = import_offline(&main, &mut s, Some(3_500_000), &seen).expect("import");
    assert_eq!(imported.addresses.unified, kv["ua"].as_str().unwrap());
    assert_eq!(backend::profile_status(&main).unwrap().network, "main");

    seen.clear();
    hardware::verify_address(&main, &mut s, kv["ua"].as_str().unwrap(), &seen.status()).expect("confirmed");
    let t_ua = key_vector("test/abandon-art/pp0/acct0")["ua"].as_str().unwrap().to_string();
    assert!(hardware::verify_address(&main, &mut s, &t_ua, &seen.status()).is_err(), "a testnet address on mainnet");

    let (_, view) = pczt_vector("main_iw_send_shielded_compact");
    let zsig = hardware::device_sign(&main, &mut s, &view, &seen.status()).expect("device signs mainnet");
    assert_eq!(Zsig::parse_for(&zsig, &view).unwrap().entries.len(), 1);
    let (_, tview) = pczt_vector("iw_send_shielded_compact");
    let e = err_text(hardware::device_sign(&main, &mut s, &tview, &seen.status()));
    assert!(hardware::explain(&e).contains("reason 0x11"), "{}", hardware::explain(&e));
    assert!(!s.lost);

    // The session was paired for mainnet: a testnet wallet cannot use it.
    seen.clear();
    for e in [
        err_text(hardware::verify_address(&test, &mut s, &t_ua, &seen.status())),
        err_text(hardware::device_sign(&test, &mut s, &tview, &seen.status()).map(|_| ())),
        err_text(import_offline(&test, &mut s, Some(3_000_000), &seen).map(|_| ())),
    ] {
        assert!(e.to_string().contains("connected for mainnet, but the wallet is now on testnet"), "{e:#}");
    }
    assert!(seen.steps().is_empty(), "something reached the device: {:?}", seen.steps());
    assert!(!test.exists(), "a testnet wallet was created from a mainnet session");

    // The extra step: a mainnet send is refused unconfirmed, before the device.
    assert!(hardware::check_mainnet_confirmed(&main, true).is_ok());
    assert!(hardware::check_mainnet_confirmed(&test, false).is_ok(), "testnet needs no confirmation");
    let e = err_text(hardware::sign_and_send(&main, &mut s, &view, false, &seen.status()));
    assert!(e.to_string().contains("mainnet transaction of real ZEC"), "{e:#}");
    assert!(seen.steps().is_empty(), "the device was asked: {:?}", seen.steps());
}

/// A board set to Testnet refuses a mainnet wallet (and the other way
/// round), and the app says which setting to change.
#[test]
fn a_board_on_the_other_network_is_explained() {
    let z = zdev_or_skip!("other-net", true);
    let d = TempDir::new("other-net");
    let main = Profile::new(Network::MainNetwork, d.0.join("wallets/main"), None);
    let seen = Seen::default();
    let port = z.port();
    let dev = link::open_usb(&port).expect("open zdev");
    let e = err_text(hardware::connect(dev, "usb", &port, 0, &main, &seen.passkey(), &seen.status()));
    assert!(zecz::device::is_code(&e, zecz::device::ERR_BOARD_NETWORK), "{e:#}");
    let why = hardware::explain(&e);
    assert!(
        why.starts_with("The board is set to Testnet, but this app is on Mainnet. Switch the board to Mainnet in Settings → Network, or use the Testnet app."),
        "{why}"
    );
    assert!(!main.exists(), "nothing written for a refused network");
}

#[test]
fn device_errors_read_as_sentences() {
    let err = |code| anyhow::Error::from(DeviceError { code, message: "dev text".into() });
    assert!(hardware::explain(&err(0x0200)).starts_with("Rejected on the device. ("));
    assert!(hardware::explain(&err(0x0201)).contains("stopped waiting"));
    assert!(hardware::explain(&err(0x0400)).contains("Connect again"));
    assert!(hardware::explain(&err(0x0100)).contains("locked"));
    let refused = hardware::explain(&err(0x050C));
    assert!(refused.contains("refused to sign") && refused.contains("0x0C"), "{refused}");
    // Anything else keeps its own words.
    assert_eq!(hardware::explain(&anyhow::anyhow!("the device did not answer")), "the device did not answer");
}

#[test]
fn propose_send_refuses_what_it_cannot_build() {
    let d = TempDir::new("propose");
    let profile = d.profile("w");
    let kv = key_vector("test/abandon-art/pp0/acct0");
    // A watch-only account: nothing could sign.
    let spec = AccountSpec::view_only(kv["ufvk"].as_str().unwrap().to_string(), "watch-only");
    offline_init(&profile, &spec, 3_000_000).unwrap();
    let to = kv["ua"].as_str().unwrap();
    let e = err_text(hardware::propose_send(&profile, to, "0.001", None));
    assert!(e.to_string().contains("watch-only"), "{e:#}");
    let e = err_text(hardware::propose_send(&profile, to, "0.123456789", None));
    assert!(format!("{e:#}").contains("at most 8 decimals"), "{e:#}");
    let e = err_text(hardware::propose_send(&profile, " ", "1", None));
    assert!(e.to_string().contains("recipient"), "{e:#}");
    assert!(!backend::profile_status(&profile).unwrap().hardware);
}

/// The whole send on testnet: import from zdev near the tip, sync, build a
/// compact send to the account's own Orchard address, sign on zdev, prove
/// and store it. It does not broadcast (set LEEKZ_TEST_BROADCAST=1 to). The
/// public test account rarely holds enough near the tip; then it reports
/// SKIPPED after the sync. LEEKZ_TEST_BIRTHDAY picks an older birthday.
#[test]
#[ignore = "needs testnet lightwalletd and a funded test account"]
fn device_send_on_testnet() {
    let z = zdev_or_skip!("testnet", true);
    let d = TempDir::new("testnet");
    let profile = d.profile("w");
    let seen = Seen::default();
    let tip = backend::tip(&profile.server).unwrap();
    let birthday = std::env::var("LEEKZ_TEST_BIRTHDAY")
        .ok()
        .and_then(|b| b.parse().ok())
        .unwrap_or(tip.height as u32 - 3 * backend::SYNC_BATCH);
    let mut s = connect(&z, &profile, &seen);
    hardware::device_import(&profile, &mut s, Some(birthday), &seen.status()).expect("import");
    backend::sync(&profile, |_| {}).expect("sync");
    let b = backend::balances(&profile).unwrap().expect("balances");
    let amount = 1_000u64;
    if b.spendable < amount + 20_000 {
        eprintln!("SKIPPED: {} zat spendable since {birthday}; not enough to send", b.spendable);
        return;
    }
    let to = backend::addresses(&profile).unwrap().orchard_only;
    let (p, view) = hardware::propose_send(&profile, &to, &format!("0.{amount:08}"), Some("leekz desktop test")).unwrap();
    assert!(p.compact);
    assert_eq!(p.payments.len(), 1);
    println!("proposal {p:?}");
    let zsig = hardware::device_sign(&profile, &mut s, &view, &seen.status()).expect("device signs");
    let broadcast = std::env::var_os("LEEKZ_TEST_BROADCAST").is_some();
    let sent = hardware::finish(&profile, &view, &zsig, broadcast, &seen.status()).expect("finish");
    println!("sent {sent:?}");
    assert_eq!(sent.txid.len(), 64);
}
