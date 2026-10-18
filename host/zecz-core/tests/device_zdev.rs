//! The device path with no board: `zdev` (firmware/sim) runs the firmware's
//! protocol, session, both signer backends and review pages on a PTY, and
//! this drives it as `zecz sign-device` does: pair, upload, review, sign.
//! The device's signatures must then prove and extract into a transaction
//! that verifies, exactly like the soft signer's.
//!
//! Builds zdev with `make` if it is missing; skips (loudly) if it cannot.

use std::io::Write;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

use pczt::Pczt;
use serde_json::Value;
use zecz::authorize::{apply_zsig, prove};
use zecz::device::{self, Device};
use zecz::zsig::Zsig;

fn repo() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn vector(name: &str) -> (Value, Vec<u8>) {
    let v = repo().join("vectors");
    let all: Vec<Value> = serde_json::from_slice(&std::fs::read(v.join("pczt.json")).unwrap()).unwrap();
    let e = all.into_iter().find(|e| e["name"] == name).expect("vector");
    let bytes = std::fs::read(v.join(e["file"].as_str().unwrap())).unwrap();
    (e, bytes)
}

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
    _dir: PathBuf,
}

impl Zdev {
    /// Settings > Network on the "board" (main or test), as the user would
    /// switch it: the pairing and everything stored stay. zdev handles the
    /// signal before it reads the next request.
    fn switch_network(&self, net: &str) {
        let sig = if net == "main" { "-USR1" } else { "-USR2" };
        let ok = Command::new("kill").arg(sig).arg(self.child.id().to_string()).status();
        assert!(ok.is_ok_and(|s| s.success()), "could not switch zdev to {net}");
    }
}

impl Drop for Zdev {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        let _ = std::fs::remove_file(&self.link);
    }
}

/// zdev on testnet (its Settings > Network), or on `net` with `start_on`.
fn start(tag: &str, auto_yes: bool) -> Option<Zdev> {
    start_on(tag, auto_yes, "test")
}

fn start_on(tag: &str, auto_yes: bool, net: &str) -> Option<Zdev> {
    let bin = zdev_binary()?;
    let dir = std::env::temp_dir().join(format!("zecz-zdev-{tag}-{}", std::process::id()));
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
    Some(Zdev { child, link, _dir: dir })
}

fn connect(z: &Zdev) -> Device {
    let mut d = Device::open(z.link.to_str().unwrap()).expect("open zdev");
    d.pair(|_| {}).expect("pair");
    d.ensure_unlocked().expect("unlocked");
    d
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

#[test]
fn device_signs_and_the_transaction_verifies() {
    use rand::{rand_core::UnwrapErr, rngs::SysRng};

    let z = zdev_or_skip!("ok", true);
    let (v, unsigned) = vector("iw_send_shielded");
    let mut dev = connect(&z);
    let (sighash, entries) = dev.z_sign(&unsigned, 0, "test", |_, _| {}).expect("device signs");
    assert_eq!(hex::encode(sighash), v["sighash"].as_str().unwrap(), "device sighash");
    assert_eq!(entries.len(), 1);

    // The .zsig round trip, then the `finish` path.
    let zsig = Zsig::parse_for(&Zsig::new(&unsigned, entries).to_bytes().unwrap(), &unsigned).unwrap();
    let signed = apply_zsig(Pczt::parse(&unsigned).unwrap(), &zsig).expect("signatures verify against rk");
    let proven = prove(signed).unwrap();
    let finalized = pczt::roles::spend_finalizer::SpendFinalizer::new(proven).finalize_spends().unwrap();
    pczt::roles::tx_extractor::TransactionExtractor::new(finalized)
        .extract(UnwrapErr(SysRng))
        .expect("extraction verifies proofs and the device's signatures");
}

/// Roadmap 10.1: the device signs transparent inputs (a shield, and a send
/// that also spends an Ironwood note); the pczt Signer accepts each
/// transparent signature against its input's sighash, and the transaction
/// proves and extracts.
#[test]
fn device_signs_transparent_inputs_and_the_transaction_verifies() {
    use rand::{rand_core::UnwrapErr, rngs::SysRng};
    use zecz::zsig::Pool;

    let z = zdev_or_skip!("transparent", true);
    let mut dev = connect(&z);
    for name in ["t_shield", "t_mixed_compact"] {
        let (v, sent) = vector(name);
        let (sighash, entries) = dev.z_sign(&sent, 0, "test", |_, _| {}).expect("device signs");
        assert_eq!(hex::encode(sighash), v["sighash"].as_str().unwrap(), "{name}: device sighash");
        let n_t = v["t_inputs"].as_array().unwrap().len();
        assert_eq!(entries.iter().filter(|e| e.pool == Pool::Transparent).count(), n_t, "{name}");

        // The full PCZT the compact view was made from takes the signatures.
        let full_name = name.trim_end_matches("_compact");
        let (_, full) = vector(full_name);
        let zsig = Zsig::new(&sent, entries);
        let bytes = zsig.to_bytes().unwrap();
        assert_eq!(bytes[4], 2, "version 2 .zsig");
        let signed = leek_android_like_apply(&full, &Zsig::parse_for(&bytes, &sent).unwrap());
        let proven = prove(signed).unwrap();
        let finalized = pczt::roles::spend_finalizer::SpendFinalizer::new(proven).finalize_spends().unwrap();
        let tx = pczt::roles::tx_extractor::TransactionExtractor::new(finalized)
            .extract(UnwrapErr(SysRng))
            .expect("extraction verifies proofs and the device's transparent signatures");
        assert_eq!(tx.transparent_bundle().unwrap().vin.len(), n_t, "{name}");
    }
}

/// `zecz finish` applies to the full PCZT; the device saw `sent`, which is
/// the same transaction (checked by the shielded sighash above).
fn leek_android_like_apply(full: &[u8], zsig: &Zsig) -> Pczt {
    apply_zsig(Pczt::parse(full).unwrap(), zsig).expect("the device's signatures verify")
}

#[test]
fn device_refuses_a_foreign_transparent_input() {
    let z = zdev_or_skip!("t-foreign", true);
    let (_, bytes) = vector("r_t_input_not_ours");
    let mut dev = connect(&z);
    let err = dev.z_sign(&bytes, 0, "test", |_, _| {}).expect_err("must refuse");
    // ZS_E_SPEND_NOT_OURS = -12.
    assert!(device::is_code(&err, device::ERR_ZSIGN_BASE | 12), "{err}");
}

#[test]
fn device_refuses_a_tampered_pczt_before_review() {
    let z = zdev_or_skip!("tampered", true);
    let (_, bytes) = vector("r_output_value_tampered");
    let mut dev = connect(&z);
    let err = dev.z_sign(&bytes, 0, "test", |_, _| {}).expect_err("must refuse");
    // ZS_E_OUTPUT_COMMITMENT = -13.
    assert!(device::is_code(&err, device::ERR_ZSIGN_BASE | 13), "{err}");
}

#[test]
fn a_rejection_on_the_device_returns_no_signature() {
    let mut z = zdev_or_skip!("reject", false);
    // Pairing: yes. Review: no.
    z.child.stdin.as_mut().unwrap().write_all(b"y\nn\n").unwrap();
    let (_, bytes) = vector("iw_send_shielded");
    let mut dev = connect(&z);
    let err = dev.z_sign(&bytes, 0, "test", |_, _| {}).expect_err("must not sign");
    assert!(device::is_code(&err, device::ERR_USER_REJECTED), "{err}");
}


#[test]
fn the_device_remembers_the_birthday_a_companion_gives_it() {
    let z = zdev_or_skip!("birthday", true);
    let mut dev = connect(&z);
    let a = dev.z_get_account(0, "test").expect("account");
    assert!(device::get_uint(&a, "birthday").is_err(), "a birthday before any was set");
    // zdev's seed is a stored wallet with no passphrase: kept in flash.
    assert!(dev.z_set_birthday(0, "test", 3_000_123).expect("set"), "not stored");
    let a = dev.z_get_account(0, "test").expect("account");
    assert_eq!(device::get_uint(&a, "birthday").unwrap(), 3_000_123);
    let a1 = dev.z_get_account(1, "test").expect("account 1");
    assert!(device::get_uint(&a1, "birthday").is_err(), "account 1 got account 0's birthday");

    // One birthday per network: a mainnet import must not replace the
    // testnet one (it once did, on the board). The board is switched to
    // Mainnet first: it answers for its own network only.
    z.switch_network("main");
    let m = dev.z_get_account(0, "main").expect("mainnet account");
    assert!(device::get_uint(&m, "birthday").is_err(), "mainnet read the testnet birthday");
    assert!(dev.z_set_birthday(0, "main", 3_520_600).expect("set main"));
    let m = dev.z_get_account(0, "main").expect("mainnet account");
    assert_eq!(device::get_uint(&m, "birthday").unwrap(), 3_520_600);
    z.switch_network("test");
    let a = dev.z_get_account(0, "test").expect("account");
    assert_eq!(device::get_uint(&a, "birthday").unwrap(), 3_000_123, "the testnet birthday was replaced");
}

#[test]
fn the_user_confirms_the_address_on_the_device() {
    let z = zdev_or_skip!("show", true);
    let mut dev = connect(&z);
    let ua = device::get_text(&dev.z_get_account(0, "test").unwrap(), "ua").unwrap();
    dev.z_show_address(0, "test", &ua).expect("confirmed");
    assert!(dev.z_show_address(0, "test", "utest1somethingelse").is_err(), "a different address passed");
}

#[test]
fn a_no_on_the_device_fails_the_address_check() {
    let mut z = zdev_or_skip!("show-no", false);
    z.child.stdin.as_mut().unwrap().write_all(b"y\nn\n").unwrap();
    let mut dev = connect(&z);
    // The wallet's own address, so the question reaches the user.
    let ua = device::get_text(&dev.z_get_account(0, "test").unwrap(), "ua").unwrap();
    let err = dev.z_show_address(0, "test", &ua).expect_err("must fail");
    assert!(device::is_code(&err, device::ERR_USER_REJECTED), "{err}");
}

#[test]
fn someone_elses_address_is_refused_before_the_screen() {
    let mut z = zdev_or_skip!("show-foreign", false);
    z.child.stdin.as_mut().unwrap().write_all(b"y\n").unwrap();
    let mut dev = connect(&z);
    // abandon-art testnet account 1: a valid address, but not account 0's.
    let keys: serde_json::Value = serde_json::from_str(
        &std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/../../vectors/keys.json")).unwrap(),
    )
    .unwrap();
    let other = keys.as_array().unwrap().iter().find(|v| v["name"] == "test/abandon-art/pp0/acct1").unwrap();
    let err = dev.z_show_address(0, "test", other["ua"].as_str().unwrap()).expect_err("must fail");
    assert!(device::is_code(&err, device::ERR_ZSIGN_BASE | 14), "{err}");
    assert!(err.to_string().contains("NOT from its wallet"), "{err}");
}

// --- mainnet (roadmap 10.3) ---

fn key_vector(name: &str) -> Value {
    let all: Vec<Value> = serde_json::from_slice(&std::fs::read(repo().join("vectors/keys.json")).unwrap()).unwrap();
    all.into_iter().find(|v| v["name"] == name).expect("key vector")
}

/// Asked for "main", the device exports the coin type 133 account (u1, t1,
/// uview1) and shows and confirms its addresses; a testnet address is not
/// an address on mainnet, and is refused before the screen.
#[test]
fn the_device_serves_the_mainnet_account() {
    let z = zdev_or_skip!("main-account", true, "main");
    let mut dev = connect(&z);
    let v = key_vector("main/abandon-art/pp0/acct0");
    let a = dev.z_get_account(0, "main").expect("account");
    assert_eq!(device::get_text(&a, "network").unwrap(), "main");
    assert_eq!(device::get_text(&a, "ufvk").unwrap(), v["ufvk"].as_str().unwrap());
    assert_eq!(device::get_text(&a, "ua").unwrap(), v["ua"].as_str().unwrap());
    assert_eq!(device::get_text(&a, "tAddress").unwrap(), v["transparent_address"].as_str().unwrap());
    assert_eq!(hex::encode(device::get_bytes(&a, "seedFingerprint").unwrap()), v["seed_fingerprint"].as_str().unwrap());

    dev.z_show_address(0, "main", v["ua"].as_str().unwrap()).expect("the default u1 address");
    dev.z_show_address(0, "main", v["ua_orchard_only"].as_str().unwrap()).expect("the Orchard-only u1 address");
    let t = key_vector("test/abandon-art/pp0/acct0");
    assert!(dev.z_show_address(0, "main", t["ua"].as_str().unwrap()).is_err(), "a testnet address shown on mainnet");
    z.switch_network("test");
    assert!(dev.z_show_address(0, "test", v["ua"].as_str().unwrap()).is_err(), "a mainnet address shown on testnet");
    z.switch_network("main");
    let m1 = key_vector("main/abandon-art/pp0/acct1");
    let err = dev.z_show_address(0, "main", m1["ua"].as_str().unwrap()).expect_err("account 1's address");
    assert!(device::is_code(&err, device::ERR_ZSIGN_BASE | 14), "{err}");
}

/// The mainnet vectors in the compact view, signed by the device for a
/// "main" request; the signatures apply to the full PCZTs, which prove and
/// extract. The same bytes are refused for "test" (code -17), and a testnet
/// PCZT for "main".
#[test]
fn the_device_signs_mainnet_and_refuses_the_other_network() {
    use rand::{rand_core::UnwrapErr, rngs::SysRng};

    let z = zdev_or_skip!("main-sign", true, "main");
    let mut dev = connect(&z);
    for name in ["main_iw_send_shielded_compact", "main_t_shield_compact"] {
        let (v, sent) = vector(name);
        assert_eq!(v["network"], "main");
        let (sighash, entries) = dev.z_sign(&sent, 0, "main", |_, _| {}).expect("device signs");
        assert_eq!(hex::encode(sighash), v["sighash"].as_str().unwrap(), "{name}: device sighash");
        let (_, full) = vector(name.trim_end_matches("_compact"));
        let zsig = Zsig::new(&sent, entries);
        let signed = leek_android_like_apply(&full, &Zsig::parse_for(&zsig.to_bytes().unwrap(), &sent).unwrap());
        let proven = prove(signed).unwrap();
        let finalized = pczt::roles::spend_finalizer::SpendFinalizer::new(proven).finalize_spends().unwrap();
        pczt::roles::tx_extractor::TransactionExtractor::new(finalized)
            .extract(UnwrapErr(SysRng))
            .expect("extraction verifies proofs and the device's signatures");
    }
    for name in ["r_main_on_test", "r_main_shield_on_test", "r_test_on_main", "r_test_shield_on_main"] {
        let (v, bytes) = vector(name);
        let net = v["network"].as_str().unwrap();
        // The board set to the network the request names, so the signer
        // itself sees the PCZT's other network.
        z.switch_network(net);
        let err = dev.z_sign(&bytes, 0, net, |_, _| {}).expect_err("must refuse");
        // ZS_E_NETWORK = -17.
        assert!(device::is_code(&err, device::ERR_ZSIGN_BASE | 17), "{name}: {err}");
    }
}

/// The board answers for its own Settings > Network only (roadmap 10.3: a
/// board set to Testnet once gave the mainnet app the mainnet account). A
/// request for the other network is refused with its own code and a message
/// naming both; switching the setting flips which one is answered.
#[test]
fn the_board_answers_only_for_its_own_network() {
    let z = zdev_or_skip!("board-network", true);
    let mut dev = connect(&z);
    let t = key_vector("test/abandon-art/pp0/acct0");
    let m = key_vector("main/abandon-art/pp0/acct0");

    // Set to Testnet: mainnet is refused, every way it can be asked.
    let refused = [
        dev.z_get_account(0, "main").map(|_| ()).expect_err("zGetAccount main"),
        dev.z_show_address(0, "main", m["ua"].as_str().unwrap()).expect_err("zShowAddress main"),
        dev.z_set_birthday(0, "main", 3_520_600).map(|_| ()).expect_err("zSetAccountMeta main"),
        dev.z_sign(&vector("main_iw_send_shielded_compact").1, 0, "main", |_, _| {}).map(|_| ()).expect_err("zSign main"),
    ];
    for err in refused {
        assert!(device::is_code(&err, device::ERR_BOARD_NETWORK), "{err:#}");
        let text = format!("{err:#}");
        assert!(text.starts_with("The board is set to Testnet, but this app is on Mainnet. Switch the board to Mainnet in Settings → Network, or use the Testnet app."), "{text}");
    }
    assert_eq!(device::get_text(&dev.z_get_account(0, "test").unwrap(), "ua").unwrap(), t["ua"].as_str().unwrap());

    // Switched to Mainnet on the board: the answers flip, same session.
    z.switch_network("main");
    assert_eq!(device::get_text(&dev.z_get_account(0, "main").unwrap(), "ua").unwrap(), m["ua"].as_str().unwrap());
    let err = dev.z_get_account(0, "test").expect_err("zGetAccount test on a mainnet board");
    assert!(device::is_code(&err, device::ERR_BOARD_NETWORK), "{err:#}");
    assert!(format!("{err:#}").starts_with("The board is set to Mainnet, but this app is on Testnet."), "{err:#}");
    assert!(device::get_uint(&dev.z_get_account(0, "main").unwrap(), "birthday").is_err(), "a refused birthday was kept");
}
