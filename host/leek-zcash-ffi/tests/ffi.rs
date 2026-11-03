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
    let a = dev.get_account(0, Network::Test).unwrap();
    assert_eq!(a.ufvk, json("keys.json", "test/abandon-art/pp0/acct0")["ufvk"].as_str().unwrap());
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
