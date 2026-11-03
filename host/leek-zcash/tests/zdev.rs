//! The kit against the simulated board: `zdev` (firmware/sim) runs the
//! firmware's protocol, session, both signer backends and review pages on a
//! PTY. A wallet's flow, end to end: features, account, address check,
//! birthday, sign_pczt, and the signed PCZT proves and extracts.
//!
//! Builds zdev with `make` if it is missing; skips (loudly) if it cannot.

mod common;

use std::io::Write;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use common::*;
use leek_device::device::Device;
use leek_zcash::{
    apply_zsig, get_account, get_features, preflight, set_birthday, show_address, sign_pczt, AddressCheck,
    CancelToken, CancellablePort, LeekError, Network, SignOptions, SignProgress, SignerView,
};

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
    let dir = std::env::temp_dir().join(format!("leek-zcash-zdev-{tag}-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let link = dir.join("port");
    let mut cmd = Command::new(bin);
    cmd.arg("--link").arg(&link).arg("--network").arg("test");
    cmd.stdin(Stdio::piped()).stdout(Stdio::null()).stderr(Stdio::null());
    if auto_yes {
        cmd.arg("--yes");
    }
    let child = cmd.spawn().ok()?;
    let deadline = Instant::now() + Duration::from_secs(10);
    while !link.exists() && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(50));
    }
    Some(Zdev { child, link })
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
fn a_wallet_imports_checks_and_signs() {
    let z = zdev_or_skip!("flow", true);
    let mut dev = connect(&z);

    let f = get_features(&mut dev).expect("features");
    assert!(f.spend_pools.iter().any(|p| p == "ironwood"), "{f:?}");
    assert_eq!(f.branch_ids, leek_zcash::KNOWN_BRANCH_IDS.to_vec());
    assert!(f.show_address);

    let k = key("test/abandon-art/pp0/acct0");
    let a = get_account(&mut dev, 0, Network::Test).expect("account");
    assert_eq!(a.ufvk, k["ufvk"].as_str().unwrap());
    assert_eq!(a.ua, k["ua"].as_str().unwrap());
    assert_eq!(a.meta(), test0());

    assert_eq!(show_address(&mut dev, 0, Network::Test, &a.ua).unwrap(), AddressCheck::Verified);
    let other = key("test/abandon-art/pp0/acct1");
    assert_eq!(show_address(&mut dev, 0, Network::Test, other["ua"].as_str().unwrap()).unwrap(), AddressCheck::NotOurs);
    assert!(set_birthday(&mut dev, 0, Network::Test, 3_000_123).expect("birthday"));
    assert_eq!(get_account(&mut dev, 0, Network::Test).unwrap().birthday, Some(3_000_123));

    for (name, view) in [
        ("iw_send_shielded", SignerView::Full),
        ("iw_send_shielded", SignerView::Compact),
        ("t_mixed", SignerView::Compact),
        ("t_shield", SignerView::Full),
    ] {
        let pczt = parse(&v(name));
        let steps = Mutex::new(Vec::new());
        let record = |p: SignProgress| steps.lock().unwrap().push(p);
        let opts = SignOptions { view, progress: Some(&record), features: Some(f.clone()), verify_account: true, ..Default::default() };
        let r = sign_pczt(&mut dev, &pczt, &a.meta(), opts).unwrap_or_else(|e| panic!("{name} {view:?}: {e}"));
        assert_eq!(hex::encode(r.sighash), vector_entry(name)["sighash"].as_str().unwrap(), "{name}");
        let steps = steps.into_inner().unwrap();
        assert_eq!(steps.first(), Some(&SignProgress::Checking));
        assert!(steps.contains(&SignProgress::ReviewOnDevice), "{steps:?}");
        assert_eq!(steps.last(), Some(&SignProgress::Applying));
        // The .zsig in the result applies again from the sent view.
        let sent = leek_zcash::signer_view_bytes(&pczt, view).unwrap();
        apply_zsig(&pczt, &sent, &r.zsig).expect("the returned .zsig applies");
        proves_and_extracts(r.signed_pczt);
    }
}

#[test]
fn refusals_are_typed() {
    let mut z = zdev_or_skip!("reject", false);
    // Pairing: yes. Review: no. Address: no.
    z.child.stdin.as_mut().unwrap().write_all(b"y\nn\nn\n").unwrap();
    let mut dev = connect(&z);
    let pczt = parse(&v("iw_send_shielded"));
    let e = sign_pczt(&mut dev, &pczt, &test0(), SignOptions::default()).unwrap_err();
    assert_eq!(e, LeekError::UserRejected);
    assert_eq!(e.recovery(), leek_zcash::Recovery::RetrySameLink);
    let ua = key("test/abandon-art/pp0/acct0")["ua"].as_str().unwrap().to_string();
    assert_eq!(show_address(&mut dev, 0, Network::Test, &ua).unwrap(), AddressCheck::Rejected);

    // Caught on the host, before any upload: another seed's PCZT, the other
    // network's.
    let zoo = meta("test/zoo-vote/pp0/acct0");
    assert_eq!(sign_pczt(&mut dev, &parse(&v("r_foreign_spend")), &zoo, SignOptions::default()).unwrap_err(), LeekError::WrongAccount);
    assert_eq!(
        sign_pczt(&mut dev, &parse(&v("r_main_on_test")), &test0(), SignOptions::default()).unwrap_err(),
        LeekError::WrongNetwork
    );
    // A PCZT the host cannot judge goes to the board, which refuses it.
    let tampered = parse(&v("r_output_value_tampered"));
    preflight(&tampered, &test0(), &leek_zcash::Features::default()).expect("not a host-side check");
    assert_eq!(sign_pczt(&mut dev, &tampered, &test0(), SignOptions::default()).unwrap_err(), LeekError::OutputMismatch);
}

#[test]
fn a_cancelled_request_stops() {
    let z = zdev_or_skip!("cancel", true);
    let token = CancelToken::new();
    let port = leek_transport_serial::SerialTransport::open(z.link.to_str().unwrap()).expect("open zdev");
    let mut dev = Device::with_port(Box::new(CancellablePort::new(port, token.clone())));
    dev.pair(|_| {}).expect("pair");
    dev.ensure_unlocked().expect("unlocked");
    let pczt = parse(&v("iw_send_shielded"));
    // Cancelled once the upload starts.
    let t = token.clone();
    let cancel_on_upload = move |p: SignProgress| {
        if matches!(p, SignProgress::Uploading { .. }) {
            t.cancel();
        }
    };
    let opts = SignOptions { progress: Some(&cancel_on_upload), cancel: Some(token.clone()), ..Default::default() };
    let e = sign_pczt(&mut dev, &pczt, &test0(), opts).unwrap_err();
    assert_eq!(e, LeekError::Cancelled);
    assert_eq!(e.recovery(), leek_zcash::Recovery::Reconnect);
}
