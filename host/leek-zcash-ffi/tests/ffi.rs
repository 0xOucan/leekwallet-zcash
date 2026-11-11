//! The FFI layer as a Kotlin or Swift host sees it, from Rust: the byte
//! API, the error mapping, and a foreign FrameTransport (both link kinds)
//! driving the simulated board (`zdev`, firmware/sim) through pairing,
//! account, signing, the SD-card calls on the same signatures, persistence
//! and cancel.
//!
//! Builds zdev with `make` if it is missing; skips (loudly) if it cannot.

use std::io::{Read, Write};
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use leek_transport_serial::SerialTransport;
use leek_zcash_ffi::*;
use serde_json::Value;

fn repo() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn v(name: &str) -> Vec<u8> {
    std::fs::read(repo().join("vectors/pczt").join(format!("{name}.pczt"))).unwrap()
}

fn json(file: &str, name: &str) -> Value {
    let all: Vec<Value> = serde_json::from_slice(&std::fs::read(repo().join("vectors").join(file)).unwrap()).unwrap();
    all.into_iter().find(|e| e["name"] == name).expect("vector")
}

fn meta(name: &str, network: Network) -> AccountMeta {
    let k = json("keys.json", name);
    AccountMeta {
        seed_fingerprint: hex::decode(k["seed_fingerprint"].as_str().unwrap()).unwrap(),
        index: 0,
        network,
        ufvk: Some(k["ufvk"].as_str().unwrap().into()),
    }
}

fn test0() -> AccountMeta {
    meta("test/abandon-art/pp0/acct0", Network::Test)
}

// ---------------------------------------------------------------- zdev

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

fn start(tag: &str) -> Option<Zdev> {
    static BUILT: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    let sim = repo().join("firmware/sim");
    let ok = *BUILT.get_or_init(|| {
        Command::new("make").arg("-C").arg(&sim).arg("build/zdev").status().is_ok_and(|s| s.success())
    });
    if !ok {
        return None;
    }
    let dir = std::env::temp_dir().join(format!("leek-zcash-ffi-zdev-{tag}-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let link = dir.join("port");
    let child = Command::new(sim.join("build/zdev"))
        .arg("--link")
        .arg(&link)
        .arg("--network")
        .arg("test")
        .arg("--yes")
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .ok()?;
    let deadline = Instant::now() + Duration::from_secs(10);
    while !link.exists() && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(50));
    }
    Some(Zdev { child, link })
}

macro_rules! zdev_or_skip {
    ($tag:expr) => {
        match start($tag) {
            Some(z) => z,
            None => {
                eprintln!("SKIPPED: zdev could not be built (make -C firmware/sim build/zdev)");
                return;
            }
        }
    };
}

/// A host transport delivering whole frames, as a BLE host does: the PTY's
/// framing is undone here and redone on the other side of the FFI.
struct FramesHost(Mutex<SerialTransport>);

impl FrameTransport for FramesHost {
    fn send(&self, f: Vec<u8>) -> Result<(), TransportFailure> {
        assert_eq!(u16::from_be_bytes([f[0], f[1]]) as usize + 2, f.len(), "one whole frame");
        self.0.lock().unwrap().send(f[2], &f[3..]).map_err(|_| TransportFailure::Disconnected)
    }

    fn recv(&self, timeout_ms: u32) -> Result<Vec<u8>, TransportFailure> {
        match self.0.lock().unwrap().recv(Duration::from_millis(timeout_ms.into())) {
            Ok((t, p)) => {
                let mut f = ((p.len() + 1) as u16).to_be_bytes().to_vec();
                f.push(t);
                f.extend_from_slice(&p);
                Ok(f)
            }
            Err(leek_transport_serial::TransportError::Timeout) => Ok(vec![]),
            Err(_) => Err(TransportFailure::Disconnected),
        }
    }
}

/// A host transport moving raw stream bytes, as a USB host does.
struct StreamHost(Mutex<Box<dyn serialport::SerialPort>>);

impl FrameTransport for StreamHost {
    fn send(&self, bytes: Vec<u8>) -> Result<(), TransportFailure> {
        self.0.lock().unwrap().write_all(&bytes).map_err(|_| TransportFailure::Disconnected)
    }

    fn recv(&self, timeout_ms: u32) -> Result<Vec<u8>, TransportFailure> {
        let mut p = self.0.lock().unwrap();
        p.set_timeout(Duration::from_millis(timeout_ms.into())).map_err(|_| TransportFailure::Disconnected)?;
        let mut buf = [0u8; 4096];
        match p.read(&mut buf) {
            Ok(n) => Ok(buf[..n].to_vec()),
            Err(e) if e.kind() == std::io::ErrorKind::TimedOut => Ok(vec![]),
            Err(_) => Err(TransportFailure::Disconnected),
        }
    }
}

struct Codes(Mutex<Vec<String>>);

impl PairingListener for Codes {
    fn on_pairing_code(&self, code: String) {
        self.0.lock().unwrap().push(code);
    }
}

fn paired(transport: Arc<dyn FrameTransport>, link: LinkKind) -> Arc<LeekDevice> {
    let d = LeekDevice::new(transport, link);
    let codes = Arc::new(Codes(Mutex::new(vec![])));
    d.pair(codes.clone()).expect("pair");
    let c = codes.0.lock().unwrap();
    assert_eq!(c.len(), 1);
    assert!(c[0].len() == 6 && c[0].chars().all(|ch| ch.is_ascii_digit()));
    d.ensure_unlocked().expect("unlocked");
    d
}

struct Steps(Mutex<Vec<SignProgress>>);

impl SignListener for Steps {
    fn on_progress(&self, p: SignProgress) {
        self.0.lock().unwrap().push(p);
    }
}

#[test]
fn a_host_signs_over_a_frames_link() {
    let z = zdev_or_skip!("frames");
    let port = SerialTransport::open(z.link.to_str().unwrap()).expect("open zdev");
    let dev = paired(Arc::new(FramesHost(Mutex::new(port))), LinkKind::Frames);

    let f = dev.get_features().unwrap();
    assert!(f.spend_pools.iter().any(|p| p == "ironwood"));
    let a = dev.get_account(0, Network::Test).unwrap();
    let k = json("keys.json", "test/abandon-art/pp0/acct0");
    assert_eq!(a.ufvk, k["ufvk"].as_str().unwrap());
    assert_eq!(hex::encode(&a.seed_fingerprint), k["seed_fingerprint"].as_str().unwrap());
    assert_eq!(a.key_source, "leekwallet");
    assert_eq!(dev.show_address(0, Network::Test, a.ua.clone()).unwrap(), AddressCheck::Verified);

    let pczt = v("iw_send_shielded");
    let steps = Arc::new(Steps(Mutex::new(vec![])));
    let r = dev.sign_pczt(pczt.clone(), test0(), SignerView::Full, true, Some(steps.clone())).unwrap();
    assert_eq!(hex::encode(&r.sighash), json("pczt.json", "iw_send_shielded")["sighash"].as_str().unwrap());
    let steps = steps.0.lock().unwrap().clone();
    assert_eq!(steps.first(), Some(&SignProgress::Checking));
    assert!(steps.iter().any(|s| matches!(s, SignProgress::Uploading { .. })));
    assert!(steps.contains(&SignProgress::ReviewOnDevice));
    assert_eq!(steps.last(), Some(&SignProgress::Applying));
    assert_eq!(r.signature_count, 1);
    assert_eq!(r.txid.len(), 32);

    // The same .zsig applies through the byte API and the SD-card calls:
    // the card file is the bytes the link carried.
    let sent = signer_view_bytes(pczt.clone(), SignerView::Full).unwrap();
    assert_eq!(apply_zsig(pczt.clone(), sent.clone(), r.zsig.clone()).unwrap(), r.signed_pczt);
    let req = prepare_sd_sign(pczt.clone(), test0(), SignerView::Full).unwrap();
    assert_eq!((req.file_name.as_str(), req.zsig_file_name.as_str()), ("LEEKZ/unsigned.pczt", "LEEKZ/signed.zsig"));
    assert_eq!(req.bytes, sent);
    let sd = finish_sd_sign(pczt, req.bytes, r.zsig.clone()).unwrap();
    assert_eq!((sd.txid_hex.as_str(), &sd.signed_pczt), (r.txid_hex.as_str(), &r.signed_pczt));

    // Persisted and read back.
    assert_eq!(sign_result_from_bytes(r.record.clone()).unwrap(), r);
}

#[test]
fn a_host_reads_the_account_over_a_stream_link() {
    let z = zdev_or_skip!("stream");
    let port = serialport::new(z.link.to_str().unwrap(), 115_200)
        .timeout(Duration::from_millis(100))
        .dtr_on_open(false)
        .open()
        .expect("open zdev");
    let dev = paired(Arc::new(StreamHost(Mutex::new(port))), LinkKind::Stream);
    let fp = dev.seed_fingerprint(Some(Network::Test)).unwrap();
    let a = dev.get_account(0, Network::Test).unwrap();
    assert_eq!(a.ufvk, json("keys.json", "test/abandon-art/pp0/acct0")["ufvk"].as_str().unwrap());
    assert_eq!(fp, a.seed_fingerprint);
    // A host-side refusal through the device: another network's PCZT.
    let e = dev.sign_pczt(v("r_main_on_test"), test0(), SignerView::Compact, false, None).unwrap_err();
    assert!(matches!(e, LeekError::Account { code: 400, recovery: Recovery::RetrySameLink, .. }), "{e:?}");
}

/// The sentences the JNI library showed, now in `reason`: the board's
/// refusals as they come back from the simulated board (set to Testnet).
#[test]
fn the_board_refusals_say_what_to_do() {
    let z = zdev_or_skip!("texts");
    let port = SerialTransport::open(z.link.to_str().unwrap()).expect("open zdev");
    let dev = paired(Arc::new(FramesHost(Mutex::new(port))), LinkKind::Frames);
    let switch = leek_zcash::leek_device::device::wrong_network_message("main");

    // A Mainnet app: the board says which way to switch, for every call
    // that names the network, before anything is derived or uploaded.
    let main0 = meta("main/abandon-art/pp0/acct0", Network::Main);
    let e = dev.get_account(0, Network::Main).unwrap_err();
    assert_eq!(e, LeekError::Account { code: 400, recovery: Recovery::RetrySameLink, reason: switch.clone() });
    assert!(e.to_string().starts_with("The board is set to Testnet, but this app is on Mainnet."), "{e}");
    assert_eq!(dev.seed_fingerprint(Some(Network::Main)).unwrap_err(), e);
    let ua = json("keys.json", "main/abandon-art/pp0/acct0")["ua"].as_str().unwrap().to_string();
    assert_eq!(dev.show_address(0, Network::Main, ua).unwrap_err(), e);
    for verify in [true, false] {
        let r = dev.sign_pczt(v("main_iw_send_shielded"), main0.clone(), SignerView::Compact, verify, None);
        assert_eq!(r.unwrap_err(), e, "verify_account {verify}");
    }
    // The PCZT on the other network than the account is the host's own
    // finding, with the general text.
    let e = dev.sign_pczt(v("r_main_on_test"), test0(), SignerView::Full, false, None).unwrap_err();
    assert!(matches!(&e, LeekError::Account { code: 400, reason, .. } if reason.as_str() != switch), "{e:?}");

    // What only an app without NU7 builds past it: preflight cannot tell
    // it from a valid PCZT, the board refuses it (0x0519), and the reason
    // is the JNI library's "Update your app" sentence.
    preflight(v("r_old_app_after_nu7"), test0(), None).expect("not a host-side check");
    let e = dev.sign_pczt(v("r_old_app_after_nu7"), test0(), SignerView::Full, true, None).unwrap_err();
    let update = leek_zcash::leek_device::device::APP_UPDATE_TEXT.to_string();
    assert_eq!(e, LeekError::Pczt { code: 520, recovery: Recovery::DoNotRetry, reason: update });
    assert!(e.to_string().starts_with("Update your app"), "{e}");

    // The session survives all of these: the board still signs.
    dev.sign_pczt(v("iw_send_shielded"), test0(), SignerView::Compact, true, None).expect("signs after the refusals");
}

/// Cancels the device once the upload starts.
struct CancelOnUpload(Mutex<Option<Arc<LeekDevice>>>);

impl SignListener for CancelOnUpload {
    fn on_progress(&self, p: SignProgress) {
        if matches!(p, SignProgress::Uploading { .. }) {
            if let Some(d) = self.0.lock().unwrap().take() {
                d.cancel();
            }
        }
    }
}

#[test]
fn a_host_cancels_mid_upload() {
    let z = zdev_or_skip!("cancel");
    let port = SerialTransport::open(z.link.to_str().unwrap()).expect("open zdev");
    let dev = paired(Arc::new(FramesHost(Mutex::new(port))), LinkKind::Frames);
    let l = Arc::new(CancelOnUpload(Mutex::new(Some(dev.clone()))));
    let e = dev.sign_pczt(v("iw_send_shielded"), test0(), SignerView::Full, false, Some(l)).unwrap_err();
    assert!(matches!(e, LeekError::Link { code: 104, recovery: Recovery::Reconnect, .. }), "{e:?}");
}

// ---------------------------------------------------------------- no board

/// A transport the platform refuses.
struct Refused;

impl FrameTransport for Refused {
    fn send(&self, _: Vec<u8>) -> Result<(), TransportFailure> {
        Err(TransportFailure::PermissionDenied)
    }
    fn recv(&self, _: u32) -> Result<Vec<u8>, TransportFailure> {
        Err(TransportFailure::PermissionDenied)
    }
}

/// A board that never answers.
struct Silent;

impl FrameTransport for Silent {
    fn send(&self, _: Vec<u8>) -> Result<(), TransportFailure> {
        Ok(())
    }
    fn recv(&self, ms: u32) -> Result<Vec<u8>, TransportFailure> {
        std::thread::sleep(Duration::from_millis(ms.into()));
        Ok(vec![])
    }
}

#[test]
fn transport_failures_keep_their_kind() {
    let d = LeekDevice::new(Arc::new(Refused), LinkKind::Stream);
    let e = d.pair(Arc::new(Codes(Mutex::new(vec![])))).unwrap_err();
    assert!(matches!(e, LeekError::Link { code: 101, recovery: Recovery::Reconnect, .. }), "{e:?}");

    // Cancelled from another thread while waiting for a board that is silent.
    let d = LeekDevice::new(Arc::new(Silent), LinkKind::Frames);
    let d2 = d.clone();
    let t = std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(300));
        d2.cancel();
    });
    let started = Instant::now();
    let e = d.pair(Arc::new(Codes(Mutex::new(vec![])))).unwrap_err();
    t.join().unwrap();
    assert!(matches!(e, LeekError::Link { code: 104, .. }), "{e:?}");
    assert!(started.elapsed() < Duration::from_secs(3));
}

#[test]
fn byte_api_errors_are_typed() {
    assert_eq!(check_broadcast(vec![0; 4], Network::Main, 3_543_405).unwrap_err().code(), 512);
    let e = signer_view_bytes(b"nope".to_vec(), SignerView::Full).unwrap_err();
    assert!(matches!(e, LeekError::Pczt { code: 512, recovery: Recovery::DoNotRetry, .. }), "{e:?}");
    assert!(e.to_string().contains("512"));
    let pczt = v("iw_send_shielded");
    let e = apply_zsig(pczt.clone(), pczt.clone(), b"ZSIG".to_vec()).unwrap_err();
    assert!(matches!(e, LeekError::Signature { code: 601, .. }), "{e:?}");
    let mut short = test0();
    short.seed_fingerprint.pop();
    assert!(matches!(preflight(pczt.clone(), short, None).unwrap_err(), LeekError::Pczt { code: 512, .. }));
    preflight(pczt, test0(), None).unwrap();
    let e = sign_result_from_bytes(b"LKSR".to_vec()).unwrap_err();
    assert_eq!(e.code(), 512);
}

#[test]
fn the_account_record_round_trips() {
    let k = json("keys.json", "test/abandon-art/pp0/acct0");
    let r = AccountRecord {
        seed_fingerprint: hex::decode(k["seed_fingerprint"].as_str().unwrap()).unwrap(),
        accounts: vec![AccountEntry {
            ufvk: k["ufvk"].as_str().unwrap().into(),
            index: 0,
            name: Some("Board".into()),
            birthday: Some(3_000_000),
        }],
    };
    let bytes = encode_account_record(r.clone()).unwrap();
    assert_eq!(decode_account_record(bytes).unwrap(), r);
    assert_eq!(account_record_type(), "leek-zcash-accounts");
}

#[test]
fn stored_app_data_seals_and_refuses() {
    // The draft's vector 1 key, as an app that remembered it would hold it.
    let key = hex::decode("f43c678ee392cc4552daa3bde21c3cb15b416811dfdacf64e7aceca6114bd0d0").unwrap();
    let k = StorageKey::from_bytes(key.clone(), "xyz.leekwallet.zcash".into(), 0, Network::Main).unwrap();
    assert_eq!(k.db_key().unwrap(), hex::decode("abd9811ecdfd468b538e7f11714bdacca99e354ea622d31f0ee3b2cee3f85973").unwrap());
    assert_eq!(k.key_bytes(), key);
    let sealed = k.seal_ufvk("uview1example".into()).unwrap();
    assert_eq!(k.open_ufvk(sealed.clone()).unwrap(), "uview1example");
    let db = k.seal_db(b"wallet.db".to_vec()).unwrap();
    assert_eq!(k.open_db(db.clone()).unwrap(), b"wallet.db");
    let mut bad = db.clone();
    *bad.last_mut().unwrap() ^= 1;
    assert!(matches!(k.open_db(bad).unwrap_err(), LeekError::Storage { code: 703, recovery: Recovery::DoNotRetry, .. }));
    // Another key: the key check refuses it (702). The same key bytes under
    // another app: the check passes, the associated data does not (703).
    let other = StorageKey::from_bytes(vec![7; 32], "xyz.leekwallet.zcash".into(), 0, Network::Main).unwrap();
    assert!(matches!(other.open_db(db.clone()).unwrap_err(), LeekError::Storage { code: 702, .. }));
    let moved = StorageKey::from_bytes(key, "com.example.wallet".into(), 0, Network::Main).unwrap();
    assert!(matches!(moved.open_db(db.clone()).unwrap_err(), LeekError::Storage { code: 703, .. }));
    k.forget();
    assert_eq!(k.open_db(db).unwrap_err().code(), 704);
    assert!(matches!(validate_app_id("Not Valid".into()).unwrap_err(), LeekError::Storage { code: 700, .. }));
    assert!(StorageKey::from_bytes(vec![0; 31], "a.b".into(), 0, Network::Main).is_err());
    assert_eq!(storage_zip_number(), 65535);
}

/// The JNI library's texts and helpers, one for one: `boardUpdateText()`,
/// `firmwareOutdated(level)`, and the refusals it reworded.
#[test]
fn the_user_facing_texts_match_the_jni() {
    use leek_zcash::leek_device::device::{wrong_network_message, APP_UPDATE_TEXT, BOARD_UPDATE_TEXT};
    assert_eq!(board_update_text(), BOARD_UPDATE_TEXT);
    assert!(board_update_text().starts_with("Update your board"));
    assert_eq!(min_firmware_compat(), 2);
    assert!(firmware_outdated(None), "a board that reports no level counts as 1");
    assert!(firmware_outdated(Some(1)));
    assert!(!firmware_outdated(Some(min_firmware_compat())));
    assert!(!firmware_outdated(Some(min_firmware_compat() + 1)));

    // ERR_APP_OUTDATED: the JNI replaced the board's text with this one.
    let e = LeekError::from(leek_zcash::LeekError::AppOutdated);
    assert_eq!(e, LeekError::Pczt { code: 520, recovery: Recovery::DoNotRetry, reason: APP_UPDATE_TEXT.into() });
    // ERR_BOARD_NETWORK: the JNI showed leek-device's sentence; so does reason.
    for (board, asked) in [(leek_zcash::Network::Test, "main"), (leek_zcash::Network::Main, "test")] {
        let e = LeekError::from(leek_zcash::LeekError::BoardNetwork { board });
        let reason = wrong_network_message(asked);
        assert_eq!(e, LeekError::Account { code: 400, recovery: Recovery::RetrySameLink, reason });
    }
    // A protocol the board does not speak, or a call it lacks: the JNI said
    // "update the older side"; reason says to update the board.
    let e = LeekError::from(leek_zcash::LeekError::FirmwareTooOld);
    assert!(matches!(&e, LeekError::Session { code: 205, recovery: Recovery::DoNotRetry, reason } if reason.contains("update")), "{e:?}");
}

/// A foreign KeyWrapper as Kotlin or Swift would implement it: XOR with a
/// pad and a trailing check byte stand in for the platform key.
struct FakeWrapper {
    hardware_backed: bool,
    deleted: Mutex<bool>,
    cancel: Mutex<bool>,
}

impl KeyWrapper for FakeWrapper {
    fn info(&self) -> KeyWrapperInfo {
        KeyWrapperInfo { hardware_backed: self.hardware_backed, user_auth_required: true, label: "Face (test)".into() }
    }

    fn wrap(&self, key: Vec<u8>) -> Result<Vec<u8>, KeyWrapperFailure> {
        let mut b: Vec<u8> = key.iter().map(|x| x ^ 0xA5).collect();
        b.push(b.iter().fold(0, |a, x| a ^ x));
        Ok(b)
    }

    fn unwrap(&self, blob: Vec<u8>) -> Result<Vec<u8>, KeyWrapperFailure> {
        if *self.cancel.lock().unwrap() {
            return Err(KeyWrapperFailure::Cancelled);
        }
        let (body, check) = blob.split_at(blob.len() - 1);
        if *self.deleted.lock().unwrap() || body.iter().fold(0, |a, x| a ^ x) != check[0] {
            return Err(KeyWrapperFailure::Invalidated);
        }
        Ok(body.iter().map(|x| x ^ 0xA5).collect())
    }

    fn delete_key(&self) -> Result<(), KeyWrapperFailure> {
        *self.deleted.lock().unwrap() = true;
        Ok(())
    }
}

#[test]
fn quick_unlock_through_a_foreign_wrapper() {
    let key = hex::decode("f43c678ee392cc4552daa3bde21c3cb15b416811dfdacf64e7aceca6114bd0d0").unwrap();
    let k = StorageKey::from_bytes(key.clone(), "xyz.leekwallet.zcash".into(), 0, Network::Main).unwrap();
    let db = k.seal_db(b"wallet.db".to_vec()).unwrap();
    let w = Arc::new(FakeWrapper { hardware_backed: true, deleted: Mutex::new(false), cancel: Mutex::new(false) });
    let q = QuickUnlock::create(k.clone(), w.clone()).unwrap();
    let q = QuickUnlock::from_bytes(q.to_bytes()).unwrap();
    assert_eq!((q.app_id(), q.account(), q.network(), q.label()), ("xyz.leekwallet.zcash".into(), 0, Network::Main, "Face (test)".into()));
    let back = q.open("xyz.leekwallet.zcash".into(), 0, Network::Main, w.clone()).unwrap();
    assert_eq!(back.key_bytes(), key);
    assert_eq!(back.open_db(db).unwrap(), b"wallet.db");
    // Another account: 702, before any prompt.
    assert!(matches!(q.open("xyz.leekwallet.zcash".into(), 1, Network::Main, w.clone()).err().unwrap(), LeekError::Storage { code: 702, .. }));
    *w.cancel.lock().unwrap() = true;
    let e = q.open("xyz.leekwallet.zcash".into(), 0, Network::Main, w.clone()).err().unwrap();
    assert!(matches!(e, LeekError::Storage { code: 708, recovery: Recovery::RetrySameLink, .. }));
    *w.cancel.lock().unwrap() = false;
    // A software key is refused (706).
    let soft = Arc::new(FakeWrapper { hardware_backed: false, deleted: Mutex::new(false), cancel: Mutex::new(false) });
    assert_eq!(QuickUnlock::create(k.clone(), soft).err().unwrap().code(), 706);
    disable_quick_unlock(w.clone()).unwrap();
    assert_eq!(q.open("xyz.leekwallet.zcash".into(), 0, Network::Main, w.clone()).err().unwrap().code(), 707);
    assert_eq!(QuickUnlock::from_bytes(b"ZAQU".to_vec()).err().unwrap().code(), 703);
    k.forget();
    assert_eq!(QuickUnlock::create(k, w).err().unwrap().code(), 704);
}

// ---------------------------------------------------------------- QR (UR)

fn record0(net: &str, birthday: Option<u32>) -> AccountRecord {
    let k = json("keys.json", &format!("{net}/abandon-art/pp0/acct0"));
    AccountRecord {
        seed_fingerprint: hex::decode(k["seed_fingerprint"].as_str().unwrap()).unwrap(),
        accounts: vec![AccountEntry { ufvk: k["ufvk"].as_str().unwrap().into(), index: 0, name: None, birthday }],
    }
}

#[test]
fn the_view_key_qr_scans_in_any_order_and_case() {
    let r = record0("test", Some(4_506_700));
    let e = QrEncoder::account_record(r.clone(), 50).unwrap();
    assert_eq!(e.ur_type(), "leek-zcash-accounts");
    assert!(e.seq_len() > 1 && !e.is_single_part());
    let frames = e.frames(0, e.seq_len() + 4);
    assert!(frames.iter().all(|f| f.starts_with("UR:LEEK-ZCASH-ACCOUNTS/") && f.len() as u32 <= e.longest_frame()));

    let s = QrScanner::new(QrScanKind::AccountRecord, Some(Network::Test));
    // Noise a camera sees: a plain address, our own request, a foreign UR.
    assert_eq!(s.feed("zcash:utest1abc".into()).state, QrPartState::Ignored);
    let pczt = QrEncoder::pczt(v("iw_send_nu7_compact"), 50).unwrap();
    assert_eq!(s.feed(pczt.frame(0)).state, QrPartState::Ignored);
    let p = s.feed("ur:crypto-psbt/lpadaxcfaxhdcxdhfleymdz".into());
    assert_eq!(p.state, QrPartState::Rejected);
    assert!(p.error.unwrap().contains("ur:crypto-psbt"));

    let mut last = None;
    for f in frames.iter().rev() {
        let p = s.feed(f.to_ascii_lowercase());
        assert!(p.percent <= 100);
        last = Some(p);
        if s.is_complete() {
            break;
        }
    }
    let last = last.unwrap();
    assert_eq!((last.state, last.percent), (QrPartState::Complete, 100));
    assert_eq!(s.account_record().unwrap(), r);
    assert_eq!(s.zsig(), None);
    // Further frames change nothing.
    assert_eq!(s.feed(frames[0].clone()).state, QrPartState::Complete);
    s.reset();
    assert!(!s.is_complete() && s.account_record().is_none());
}

#[test]
fn the_view_key_of_the_other_network_is_refused() {
    let e = QrEncoder::account_record(record0("main", None), 50).unwrap();
    let s = QrScanner::new(QrScanKind::AccountRecord, Some(Network::Test));
    let mut refused = None;
    for i in 0..(e.seq_len() as u64 * 3) {
        let p = s.feed(e.frame(i));
        if p.state == QrPartState::Refused {
            refused = Some(p);
            break;
        }
    }
    let p = refused.expect("refused");
    assert!(p.error.unwrap().contains("Mainnet"), "names the key's network");
    assert!(!s.is_complete() && p.total == 0, "started over");
    // No network given: any record is taken.
    let any = QrScanner::new(QrScanKind::AccountRecord, None);
    for i in 0..(e.seq_len() as u64 * 3) {
        any.feed(e.frame(i));
    }
    assert!(any.is_complete());
}

#[test]
fn the_board_reply_qr_gives_the_zsig() {
    let zsig: Vec<u8> = (0..700u32).map(|i| (i * 31 % 251) as u8).collect();
    let e = QrEncoder::zsig(zsig.clone(), 50).unwrap();
    let s = QrScanner::new(QrScanKind::Zsig, None);
    // A reflection of the request on the host's own screen is ignored.
    let pczt = QrEncoder::pczt(v("iw_send_nu7_compact"), 50).unwrap();
    assert_eq!(s.feed(pczt.frame(3)).state, QrPartState::Ignored);
    let acc = QrEncoder::account_record(record0("test", None), 50).unwrap();
    assert_eq!(s.feed(acc.frame(0)).state, QrPartState::Rejected);
    // Skip every third frame: the fountain parts fill the gaps.
    let mut i = 0u64;
    while !s.is_complete() && i < 500 {
        if i % 3 != 1 {
            s.feed(e.frame(i));
        }
        i += 1;
    }
    assert_eq!(s.zsig().unwrap(), zsig);
    assert!(s.account_record().is_none());
}

#[test]
fn the_scanner_follows_a_sender_that_switched() {
    let a = QrEncoder::account_record(record0("test", None), 40).unwrap();
    let b = QrEncoder::account_record(record0("test", Some(4_000_000)), 60).unwrap();
    let s = QrScanner::new(QrScanKind::AccountRecord, None);
    s.feed(a.frame(0));
    s.feed(a.frame(1));
    // One stray part of another message is refused...
    assert_eq!(s.feed(b.frame(0)).state, QrPartState::Rejected);
    assert_eq!(s.feed(a.frame(2)).state, QrPartState::Accepted);
    // ...three in a row and the scanner follows the new one.
    for i in 0..3 {
        s.feed(b.frame(i));
    }
    let p = s.feed(b.frame(3));
    assert_eq!(p.total, b.seq_len());
    for i in 4..(b.seq_len() as u64 * 3) {
        if s.is_complete() {
            break;
        }
        s.feed(b.frame(i));
    }
    assert_eq!(s.account_record().unwrap().accounts[0].birthday, Some(4_000_000));
}

#[test]
fn the_signing_request_qr_matches_the_companion() {
    let e = QrEncoder::pczt(v("iw_send_nu7_compact"), qr_defaults().fragment_len).unwrap();
    // The board-tested size: 28 fragments of 50 B (app/src-tauri airgap.rs).
    assert_eq!((e.seq_len(), e.fragment_len()), (28, 50));
    let f = e.frames(0, 40);
    assert_eq!(f.len(), 40);
    assert!(f[0].starts_with("UR:LEEK-PCZT/1-28/"));
    assert_eq!(f[39], e.frame(39));
    assert_eq!(e.frames(0, 10_000).len() as u32, QR_MAX_BATCH);
    assert!(QrEncoder::pczt(b"not a pczt".to_vec(), 50).is_err());
    assert_eq!(qr_part_type(f[0].clone()).as_deref(), Some("leek-pczt"));
    assert_eq!(qr_part_type("zcash:utest1".into()), None);

    let d = qr_defaults();
    assert_eq!((d.fragment_len, d.frame_ms, d.board_zoom), (50, 500, 2));
    assert!(d.fragment_choices.contains(&40) && d.frame_ms_choices.contains(&500));
}
