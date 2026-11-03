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

fn test0() -> AccountMeta {
    let k = json("keys.json", "test/abandon-art/pp0/acct0");
    AccountMeta {
        seed_fingerprint: hex::decode(k["seed_fingerprint"].as_str().unwrap()).unwrap(),
        index: 0,
        network: Network::Test,
        ufvk: Some(k["ufvk"].as_str().unwrap().into()),
    }
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
