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

impl Drop for Zdev {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        let _ = std::fs::remove_file(&self.link);
    }
}

fn start(tag: &str, auto_yes: bool) -> Option<Zdev> {
    let bin = zdev_binary()?;
    let dir = std::env::temp_dir().join(format!("zecz-zdev-{tag}-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let link = dir.join("port");
    let mut cmd = Command::new(bin);
    cmd.arg("--link").arg(&link).stdin(Stdio::piped()).stdout(Stdio::null()).stderr(Stdio::null());
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
        match start($tag, $yes) {
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
    assert!(dev.z_set_birthday(0, 3_000_123).expect("set"), "not stored");
    let a = dev.z_get_account(0, "test").expect("account");
    assert_eq!(device::get_uint(&a, "birthday").unwrap(), 3_000_123);
    let a1 = dev.z_get_account(1, "test").expect("account 1");
    assert!(device::get_uint(&a1, "birthday").is_err(), "account 1 got account 0's birthday");
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
