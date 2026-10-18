//! The LeekWallet device over USB serial (or `zdev`'s PTY): the v2 pairing
//! handshake, the encrypted session, and the Zcash calls.
//!
//! Host half of `firmware/src/session.c` and `protocol.c`, written from
//! docs/PROTOCOL.md §3 and checked against `app/packages/core/src/session.ts`:
//! X25519, a device commitment opened after the host reveals its nonce, HKDF
//! over the whole transcript into two directional keys and a six-digit
//! passkey, then ChaCha20-Poly1305 with counter nonces.

use std::time::{Duration, Instant};

use anyhow::{anyhow, bail, Context, Result};
use chacha20poly1305::aead::{Aead, KeyInit};
use chacha20poly1305::{ChaCha20Poly1305, Key, Nonce};
// Re-exported: the replies of `call` and `z_get_account` are CBOR values.
pub use ciborium::value::Value;
use hkdf::Hkdf;
use leek_transport_serial::{
    TransportError, FRAME_ENC_REQUEST, FRAME_ENC_RESPONSE, FRAME_ERROR,
    FRAME_REQUEST, FRAME_RESPONSE,
};
use sha2::{Digest, Sha256};
use x25519_dalek::{PublicKey, StaticSecret};

use crate::zsig::{Entry, Pool};

/// Encrypted error frame (the device's 0x7E).
const FRAME_ENC_ERROR: u8 = 0x7E;

pub const PROTOCOL_VERSION: u64 = 2;
const NONCE_BYTES: usize = 16;

const LABEL_H2D: &[u8] = b"leek-session-h2d-v2";
const LABEL_D2H: &[u8] = b"leek-session-d2h-v2";
const LABEL_PASSKEY: &[u8] = b"leek-session-passkey-v2";
const LABEL_COMMIT: &[u8] = b"leek-session-commit-v2";
const LABEL_TRANSCRIPT: &[u8] = b"leek-session-transcript-v2";

/// Error codes the device sends (protocol.c).
pub const ERR_NOT_UNLOCKED: u64 = 0x0100;
pub const ERR_USER_REJECTED: u64 = 0x0200;
pub const ERR_USER_TIMEOUT: u64 = 0x0201;
pub const ERR_SESSION: u64 = 0x0400;
pub const ERR_BUSY: u64 = 0x0401;
pub const ERR_ZSIGN_BASE: u64 = 0x0500;
/// The request named the other network than the board's own setting
/// (Settings > Network on the board): refused before anything was derived.
pub const ERR_BOARD_NETWORK: u64 = ERR_ZSIGN_BASE | 40;

/// An error frame from the device.
#[derive(Debug)]
pub struct DeviceError {
    pub code: u64,
    pub message: String,
}

impl std::fmt::Display for DeviceError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.code & 0xFF00 == ERR_ZSIGN_BASE && self.code != ERR_BOARD_NETWORK {
            write!(f, "device refused to sign: {} (code -{})", self.message, self.code & 0xFF)
        } else {
            write!(f, "device error 0x{:04X}: {}", self.code, self.message)
        }
    }
}

impl std::error::Error for DeviceError {}

// ------------------------------------------------------------------ crypto

/// H(label ‖ PKb ‖ PKa ‖ Nb), the device's commitment to its nonce.
pub fn commitment(device_pub: &[u8; 32], host_pub: &[u8; 32], device_nonce: &[u8]) -> [u8; 32] {
    let mut h = Sha256::new();
    h.update(LABEL_COMMIT);
    h.update(device_pub);
    h.update(host_pub);
    h.update(device_nonce);
    h.finalize().into()
}

/// Directional keys and the passkey, from the X25519 secret and the transcript.
pub struct Keys {
    pub h2d: [u8; 32],
    pub d2h: [u8; 32],
    pub passkey: String,
}

pub fn derive(shared: &[u8; 32], host_pub: &[u8; 32], device_pub: &[u8; 32],
              host_nonce: &[u8], device_nonce: &[u8]) -> Keys {
    let mut t = Sha256::new();
    t.update(LABEL_TRANSCRIPT);
    t.update(host_pub);
    t.update(device_pub);
    t.update(host_nonce);
    t.update(device_nonce);
    let salt: [u8; 32] = t.finalize().into();
    let hk = Hkdf::<Sha256>::new(Some(&salt), shared);
    let okm = |label: &[u8]| {
        let mut out = [0u8; 32];
        hk.expand(label, &mut out).expect("32 bytes is a valid HKDF length");
        out
    };
    let p = okm(LABEL_PASSKEY);
    let n = u32::from_be_bytes([p[0], p[1], p[2], p[3]]) % 1_000_000;
    Keys { h2d: okm(LABEL_H2D), d2h: okm(LABEL_D2H), passkey: format!("{n:06}") }
}

/// What to tell the user when the board refused `asked` (a request's
/// "network", "main" or "test") because it is set to the other network.
pub fn wrong_network_message(asked: &str) -> String {
    let (app, board) = if asked == "main" { ("Mainnet", "Testnet") } else { ("Testnet", "Mainnet") };
    format!(
        "The board is set to {board}, but this app is on {app}. Switch the board to {app} in \
         Settings → Network, or use the {board} app."
    )
}

/// 4 zero bytes ‖ 8-byte big-endian counter, as the firmware builds it.
fn nonce_for(counter: u64) -> [u8; 12] {
    let mut n = [0u8; 12];
    n[4..].copy_from_slice(&counter.to_be_bytes());
    n
}

/// An established session. The host advances its send counter only when a
/// reply proves the device accepted the frame, so an identical request can be
/// resent while the user has not yet pressed ALLOW (session.ts).
struct Session {
    send: ChaCha20Poly1305,
    recv: ChaCha20Poly1305,
    tx: u64,
    rx: u64,
}

impl Session {
    fn seal(&self, plain: &[u8]) -> Result<Vec<u8>> {
        self.send.encrypt(Nonce::from_slice(&nonce_for(self.tx)), plain).map_err(|_| anyhow!("seal"))
    }

    fn open(&mut self, ct: &[u8]) -> Result<Vec<u8>> {
        let out = self
            .recv
            .decrypt(Nonce::from_slice(&nonce_for(self.rx)), ct)
            .map_err(|_| anyhow!("a device reply failed authentication; the channel is not trustworthy"))?;
        self.rx += 1;
        self.tx += 1;
        Ok(out)
    }
}

// -------------------------------------------------------------------- CBOR

fn cbor(v: &Value) -> Result<Vec<u8>> {
    let mut out = Vec::new();
    ciborium::ser::into_writer(v, &mut out)?;
    Ok(out)
}

fn request(method: &str, fields: Vec<(&str, Value)>) -> Value {
    let mut m = vec![(Value::Text("method".into()), Value::Text(method.into()))];
    m.extend(fields.into_iter().map(|(k, v)| (Value::Text(k.into()), v)));
    Value::Map(m)
}

pub fn get<'a>(v: &'a Value, key: &str) -> Option<&'a Value> {
    match v {
        Value::Map(m) => m.iter().find(|(k, _)| matches!(k, Value::Text(t) if t == key)).map(|(_, v)| v),
        _ => None,
    }
}

pub fn get_bytes(v: &Value, key: &str) -> Result<Vec<u8>> {
    match get(v, key) {
        Some(Value::Bytes(b)) => Ok(b.clone()),
        _ => bail!("reply has no byte string {key:?}"),
    }
}

pub fn get_text(v: &Value, key: &str) -> Result<String> {
    match get(v, key) {
        Some(Value::Text(t)) => Ok(t.clone()),
        _ => bail!("reply has no text {key:?}"),
    }
}

pub fn get_uint(v: &Value, key: &str) -> Result<u64> {
    match get(v, key) {
        Some(Value::Integer(i)) => u64::try_from(i128::from(*i)).map_err(|_| anyhow!("{key} negative")),
        _ => bail!("reply has no integer {key:?}"),
    }
}

/// `{result: …}` → the result, `{code, message}` → a DeviceError.
fn decode_reply(frame_type: u8, body: &[u8]) -> Result<Value> {
    let v: Value = ciborium::de::from_reader(body).context("device reply is not CBOR")?;
    if frame_type == FRAME_ERROR || frame_type == FRAME_ENC_ERROR {
        let code = get_uint(&v, "code").unwrap_or(0);
        let message = get_text(&v, "message").unwrap_or_default();
        return Err(DeviceError { code, message }.into());
    }
    get(&v, "result").cloned().ok_or_else(|| anyhow!("device reply has no result"))
}

// ------------------------------------------------------------------ device

/// One framed exchange with the device. The desktop serial port implements
/// it here; the Android app implements it over USB OTG from Kotlin (JNI).
/// `recv` returns `TransportError::Timeout` when nothing complete arrived.
pub trait FramePort: Send {
    fn send(&mut self, frame_type: u8, payload: &[u8]) -> Result<(), TransportError>;
    fn recv(&mut self, timeout: Duration) -> Result<(u8, Vec<u8>), TransportError>;
}

#[cfg(feature = "serial")]
impl FramePort for leek_transport_serial::SerialTransport {
    fn send(&mut self, frame_type: u8, payload: &[u8]) -> Result<(), TransportError> {
        leek_transport_serial::SerialTransport::send(self, frame_type, payload)
    }
    fn recv(&mut self, timeout: Duration) -> Result<(u8, Vec<u8>), TransportError> {
        leek_transport_serial::SerialTransport::recv(self, timeout)
    }
}

pub struct Device {
    port: Box<dyn FramePort>,
    session: Option<Session>,
}

/// Replies can take as long as a person reading the review screens.
const USER_TIMEOUT: Duration = Duration::from_secs(150);
const SHORT: Duration = Duration::from_secs(10);
/// Requests that derive keys on the device. On the S3 at 160 MHz an Orchard
/// account (Sinsemilla, in software) takes many seconds, and the device may
/// first wait up to 10 s for its vault after the PIN; 10 s was not enough on
/// the first board.
const DERIVE: Duration = Duration::from_secs(90);

impl Device {
    /// The device on a serial port (a board, or zdev's PTY).
    #[cfg(feature = "serial")]
    pub fn open(path: &str) -> Result<Self> {
        let port = leek_transport_serial::SerialTransport::open(path).map_err(|e| anyhow!("opening {path}: {e}"))?;
        Ok(Self::with_port(Box::new(port)))
    }

    /// The device over any framed transport.
    pub fn with_port(port: Box<dyn FramePort>) -> Self {
        Self { port, session: None }
    }

    fn roundtrip(&mut self, frame_type: u8, payload: &[u8], timeout: Duration) -> Result<(u8, Vec<u8>)> {
        self.port.send(frame_type, payload).map_err(|e| anyhow!("send: {e}"))?;
        self.port.recv(timeout).map_err(|e| match e {
            TransportError::Timeout => anyhow!("the device did not answer (is it connected, and is this its port?)"),
            e => anyhow!("receive: {e}"),
        })
    }

    fn plain(&mut self, req: &Value, timeout: Duration) -> Result<Value> {
        let (t, body) = self.roundtrip(FRAME_REQUEST, &cbor(req)?, timeout)?;
        if t != FRAME_RESPONSE && t != FRAME_ERROR {
            bail!("unexpected frame type 0x{t:02X} to a plaintext request");
        }
        decode_reply(t, &body)
    }

    /// Pair: the v2 handshake, then wait for the user to compare the passkey
    /// and press ALLOW on the device. `show` receives the six digits.
    pub fn pair(&mut self, show: impl Fn(&str)) -> Result<()> {
        let mut sk = [0u8; 32];
        getrandom::fill(&mut sk).map_err(|e| anyhow!("rng: {e}"))?;
        let secret = StaticSecret::from(sk);
        let host_pub = PublicKey::from(&secret).to_bytes();

        let ack = loop {
            let hello = request("hello", vec![
                ("version", Value::Integer(PROTOCOL_VERSION.into())),
                ("hostPubkey", Value::Bytes(host_pub.to_vec())),
            ]);
            match self.plain(&hello, SHORT) {
                Err(e) if is_code(&e, ERR_BUSY) => {
                    eprintln!("The device is waiting for the user; retrying ...");
                    std::thread::sleep(Duration::from_secs(1));
                }
                r => break r?,
            }
        };
        if get_uint(&ack, "version")? != PROTOCOL_VERSION {
            bail!("the device speaks a different protocol version; update the older side");
        }
        let device_pub: [u8; 32] = get_bytes(&ack, "devicePubkey")?
            .try_into().map_err(|_| anyhow!("devicePubkey is not 32 bytes"))?;
        let commit = get_bytes(&ack, "deviceCommit")?;

        let mut host_nonce = [0u8; NONCE_BYTES];
        getrandom::fill(&mut host_nonce).map_err(|e| anyhow!("rng: {e}"))?;
        let reveal = request("helloReveal", vec![("hostNonce", Value::Bytes(host_nonce.to_vec()))]);
        let r = self.plain(&reveal, SHORT)?;
        let device_nonce = get_bytes(&r, "deviceNonce")?;
        if device_nonce.len() != NONCE_BYTES {
            bail!("deviceNonce is not {NONCE_BYTES} bytes");
        }
        // Not optional: an unverified nonce is one a relay could choose after
        // seeing everything else (PROTOCOL.md §3).
        if commitment(&device_pub, &host_pub, &device_nonce) != commit.as_slice() {
            bail!("the device's commitment does not open: refusing to pair (possible relay)");
        }

        let shared = secret.diffie_hellman(&PublicKey::from(device_pub));
        if !shared.was_contributory() {
            bail!("degenerate shared secret: the device key is small-order");
        }
        let keys = derive(shared.as_bytes(), &host_pub, &device_pub, &host_nonce, &device_nonce);
        show(&keys.passkey);
        self.session = Some(Session {
            send: ChaCha20Poly1305::new(Key::from_slice(&keys.h2d)),
            recv: ChaCha20Poly1305::new(Key::from_slice(&keys.d2h)),
            tx: 0,
            rx: 0,
        });

        // The same sealed ping, resent until the user presses ALLOW: the
        // device answers "decrypt failed" in plaintext and moves no counter
        // until then.
        let ping = cbor(&request("ping", vec![]))?;
        let sealed = self.session.as_ref().unwrap().seal(&ping)?;
        let deadline = Instant::now() + USER_TIMEOUT;
        loop {
            let (t, body) = self.roundtrip(FRAME_ENC_REQUEST, &sealed, SHORT)?;
            match t {
                FRAME_ENC_RESPONSE => {
                    let plain = self.session.as_mut().unwrap().open(&body)?;
                    decode_reply(FRAME_RESPONSE, &plain)?;
                    return Ok(());
                }
                FRAME_ERROR if Instant::now() < deadline => std::thread::sleep(Duration::from_millis(500)),
                FRAME_ERROR => bail!("the pairing was not confirmed on the device in time"),
                t => bail!("unexpected frame type 0x{t:02X} while pairing"),
            }
        }
    }

    /// One encrypted request and its reply.
    pub fn call(&mut self, method: &str, fields: Vec<(&str, Value)>, timeout: Duration) -> Result<Value> {
        // Kept for the board's wrong-network refusal, which is worded here.
        let asked = fields.iter().find_map(|(k, v)| match (*k, v) {
            ("network", Value::Text(n)) => Some(n.clone()),
            _ => None,
        });
        self.call_inner(method, fields, timeout).map_err(|e| match &asked {
            // Context keeps the DeviceError underneath, so is_code still works.
            Some(n) if is_code(&e, ERR_BOARD_NETWORK) => e.context(wrong_network_message(n)),
            _ => e,
        })
    }

    fn call_inner(&mut self, method: &str, fields: Vec<(&str, Value)>, timeout: Duration) -> Result<Value> {
        let plain = cbor(&request(method, fields))?;
        let sealed = self.session.as_ref().ok_or_else(|| anyhow!("not paired"))?.seal(&plain)?;
        let debug = std::env::var_os("ZECZ_DEBUG").is_some();
        if debug {
            eprintln!("[zecz] -> {method} ({} bytes, timeout {}s)", sealed.len(), timeout.as_secs());
        }
        let (t, body) = self.roundtrip(FRAME_ENC_REQUEST, &sealed, timeout)?;
        if debug {
            eprintln!("[zecz] <- frame 0x{t:02X} ({} bytes)", body.len());
        }
        match t {
            FRAME_ENC_RESPONSE | FRAME_ENC_ERROR => {
                let opened = self.session.as_mut().unwrap().open(&body)?;
                decode_reply(t, &opened)
            }
            // Plaintext: the frame never decrypted, so no counter moved.
            FRAME_ERROR => decode_reply(t, &body),
            t => bail!("unexpected frame type 0x{t:02X}"),
        }
    }

    /// Ask the device to prompt for its PIN if it is locked, and wait until
    /// the user has entered it there (the PIN never crosses the wire).
    pub fn ensure_unlocked(&mut self) -> Result<()> {
        let r = self.call("unlock", vec![], SHORT)?;
        if get_uint(&r, "unlocked").unwrap_or(0) == 1 {
            return Ok(());
        }
        eprintln!("Enter the PIN on the device ...");
        let deadline = Instant::now() + USER_TIMEOUT;
        while Instant::now() < deadline {
            std::thread::sleep(Duration::from_secs(1));
            let s = self.call("getStatus", vec![], SHORT)?;
            if get_uint(&s, "unlocked").unwrap_or(0) == 1 {
                return Ok(());
            }
        }
        bail!("the device was not unlocked in time")
    }

    /// `zGetAccount`: UFVK, UA, t-address and ZIP 32 seed fingerprint.
    pub fn z_get_account(&mut self, account: u32, network: &str) -> Result<Value> {
        self.call("zGetAccount", vec![
            ("account", Value::Integer(account.into())),
            ("network", Value::Text(network.into())),
        ], DERIVE)
    }

    /// `zShowAddress`: the device shows its own unified address for the user
    /// to compare with `expected_ua`, and answers only once they say it
    /// matches. Errors if they say it does not, or if the device's address
    /// differs from the one expected.
    pub fn z_show_address(&mut self, account: u32, network: &str, expected_ua: &str) -> Result<()> {
        // The app's address goes along: wallets rotate diversified addresses,
        // and the device shows this one only after its keys prove it is the
        // account's. Firmware without that check ignores the field and shows
        // its default address, which the comparison below then catches.
        let r = self.call("zShowAddress", vec![
            ("account", Value::Integer(account.into())),
            ("network", Value::Text(network.into())),
            ("ua", Value::Text(expected_ua.into())),
        ], USER_TIMEOUT).map_err(|e| {
            // Context keeps the DeviceError underneath, so is_code still works.
            let why = match e.downcast_ref::<DeviceError>().map(|d| d.code) {
                Some(c) if c == ERR_ZSIGN_BASE | 14 => {
                    "The device says this address is NOT from its wallet. Do not use it to receive: this app may be compromised."
                }
                Some(c) if c == ERR_ZSIGN_BASE | 16 => {
                    "This address has a receiver the device cannot check (for example Sapling), so it was not shown."
                }
                Some(ERR_USER_REJECTED) => "You answered on the device that the addresses do not match.",
                _ => return e,
            };
            e.context(why)
        })?;
        let ua = get_text(&r, "ua")?;
        if ua != expected_ua {
            bail!("the device confirmed {ua}, but this wallet expects {expected_ua}");
        }
        Ok(())
    }

    /// `zSetAccountMeta`: record the account's birthday on the device, so
    /// the next companion can import without asking. True if the device
    /// stored it in flash (a stored wallet with no passphrase), false if it
    /// holds it until lock. The device keeps one birthday per network.
    pub fn z_set_birthday(&mut self, account: u32, network: &str, birthday: u32) -> Result<bool> {
        let r = self.call("zSetAccountMeta", vec![
            ("account", Value::Integer(account.into())),
            ("network", Value::Text(network.into())),
            ("birthday", Value::Integer(birthday.into())),
        ], DERIVE)?;
        Ok(get_uint(&r, "stored")? == 1)
    }

    /// Upload a PCZT, let the user review it on the device, and return the
    /// sighash they approved with the device's signatures.
    pub fn z_sign(&mut self, pczt: &[u8], account: u32, network: &str,
                  progress: impl Fn(usize, usize)) -> Result<([u8; 32], Vec<Entry>)> {
        let sha: [u8; 32] = Sha256::digest(pczt).into();
        let begin = self.call("zSignBegin", vec![
            ("len", Value::Integer((pczt.len() as u64).into())),
            ("sha256", Value::Bytes(sha.to_vec())),
            ("account", Value::Integer(account.into())),
            ("network", Value::Text(network.into())),
        ], DERIVE)?;
        let max = get_uint(&begin, "maxChunk")? as usize;
        if max == 0 {
            bail!("device offered a zero chunk size");
        }
        let mut off = 0;
        while off < pczt.len() {
            let n = max.min(pczt.len() - off);
            let r = self.call("zSignChunk", vec![
                ("offset", Value::Integer((off as u64).into())),
                ("data", Value::Bytes(pczt[off..off + n].to_vec())),
            ], SHORT)?;
            off += n;
            if get_uint(&r, "received")? as usize != off {
                bail!("device received count disagrees after {off} bytes");
            }
            progress(off, pczt.len());
        }
        let done = self.call("zSignFinish", vec![], USER_TIMEOUT)?;
        let sighash: [u8; 32] = get_bytes(&done, "sighash")?
            .try_into().map_err(|_| anyhow!("sighash is not 32 bytes"))?;
        let sigs = match get(&done, "signatures") {
            Some(Value::Array(a)) => a,
            _ => bail!("reply has no signatures"),
        };
        let mut entries = Vec::with_capacity(sigs.len());
        for s in sigs {
            let index = get_uint(s, "index")? as u32;
            let pool = match get_uint(s, "pool")? {
                0 => Pool::Transparent,
                1 => Pool::Orchard,
                2 => Pool::Ironwood,
                p => bail!("device returned a signature for pool {p}, which this host cannot apply"),
            };
            if pool == Pool::Transparent {
                // DER || sighash type, and the key (PROTOCOL.md, roadmap 10.1).
                let sig = get_bytes(s, "sig")?;
                if !(9..=crate::zsig::TSIG_MAX).contains(&sig.len()) {
                    bail!("transparent signature of {} bytes", sig.len());
                }
                let pubkey: [u8; 33] = get_bytes(s, "pubkey")?
                    .try_into().map_err(|_| anyhow!("pubkey is not 33 bytes"))?;
                entries.push(Entry::transparent(index, sig, pubkey));
                continue;
            }
            let sig: [u8; 64] = get_bytes(s, "sig")?
                .try_into().map_err(|_| anyhow!("signature is not 64 bytes"))?;
            entries.push(Entry::shielded(pool, index, sig));
        }
        Ok((sighash, entries))
    }
}

pub fn is_code(e: &anyhow::Error, code: u64) -> bool {
    e.downcast_ref::<DeviceError>().is_some_and(|d| d.code == code)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_wrong_network_is_named_both_ways() {
        let m = wrong_network_message("main");
        assert!(m.starts_with("The board is set to Testnet, but this app is on Mainnet."), "{m}");
        assert!(m.contains("Switch the board to Mainnet in Settings → Network, or use the Testnet app."), "{m}");
        let t = wrong_network_message("test");
        assert!(t.starts_with("The board is set to Mainnet, but this app is on Testnet."), "{t}");
        assert!(t.contains("or use the Mainnet app."), "{t}");
        let e = DeviceError { code: ERR_BOARD_NETWORK, message: "x".into() };
        assert_eq!(e.to_string(), "device error 0x0528: x");
    }

    #[test]
    fn nonce_layout_matches_the_firmware() {
        assert_eq!(nonce_for(0), [0; 12]);
        assert_eq!(nonce_for(0x0102_0304), [0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4]);
    }

    #[test]
    fn passkey_is_six_digits_and_bound_to_every_input() {
        let s = [7u8; 32];
        let (a, b) = ([1u8; 32], [2u8; 32]);
        let k = derive(&s, &a, &b, &[3; 16], &[4; 16]);
        assert_eq!(k.passkey.len(), 6);
        assert_ne!(k.h2d, k.d2h);
        assert_ne!(derive(&s, &a, &b, &[3; 16], &[5; 16]).passkey, k.passkey);
        assert_ne!(derive(&s, &b, &a, &[3; 16], &[4; 16]).h2d, k.h2d);
    }
}
