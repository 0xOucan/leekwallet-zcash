//! `.zkv`: a seed on an SD card, sealed under a passphrase.
//!
//! An independent implementation of the device's format
//! (firmware/src/zkv.h, docs/zcash/FORMATS.md), written from the spec and
//! not from the C: both must produce the golden file in vectors/zkv/
//! byte for byte, and each opens what the other seals.
//!
//! Header (38 bytes, GCM associated data): "LKZV" ‖ version 1 ‖ KDF 1
//! (PBKDF2-HMAC-SHA512) ‖ iterations u32 LE ‖ salt[16] ‖ nonce[12].
//! Then AES-256-GCM(payload) ‖ tag[16]. Key: the first 32 bytes of
//! PBKDF2-HMAC-SHA512(passphrase, "LeekWallet zkv v1" ‖ salt, iterations).
//! Payload v1: 1 ‖ entropy_len ‖ entropy ‖ birthday u32 LE ‖ account u32 LE
//! ‖ flags ‖ label_len ‖ label.

use aes_gcm::aead::{Aead, KeyInit, Payload};
use aes_gcm::{Aes256Gcm, Nonce};
use anyhow::{anyhow, bail, Result};
use sha2::Sha512;
use secrecy::zeroize::Zeroize;

pub const MAGIC: &[u8; 4] = b"LKZV";
pub const HEADER: usize = 38;
pub const TAG: usize = 16;
pub const ITERATIONS_DEFAULT: u32 = 20_000;
pub const ITERATIONS_MIN: u32 = 10_000;
pub const ITERATIONS_MAX: u32 = 200_000;
pub const LABEL_MAX: usize = 32;
pub const FLAG_USE_QSK: u8 = 0x01;
const KDF_DOMAIN: &[u8] = b"LeekWallet zkv v1";

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Contents {
    pub entropy: Vec<u8>,
    pub birthday: u32,
    pub account: u32,
    pub flags: u8,
    pub label: String,
}

impl Drop for Contents {
    fn drop(&mut self) {
        self.entropy.zeroize();
    }
}

fn printable(s: &[u8]) -> bool {
    s.iter().all(|&c| (0x20..=0x7e).contains(&c))
}

/// Same rule as the device: 8-64 printable ASCII characters.
pub fn pass_ok(pass: &str) -> bool {
    (8..=64).contains(&pass.len()) && printable(pass.as_bytes())
}

fn key(pass: &str, iterations: u32, salt: &[u8]) -> [u8; 32] {
    let mut s = KDF_DOMAIN.to_vec();
    s.extend_from_slice(salt);
    let mut full = [0u8; 64];
    pbkdf2::pbkdf2_hmac::<Sha512>(pass.as_bytes(), &s, iterations, &mut full);
    let mut k = [0u8; 32];
    k.copy_from_slice(&full[..32]);
    full.zeroize();
    k
}

fn check_contents(c: &Contents) -> Result<()> {
    let n = c.entropy.len();
    if !(16..=32).contains(&n) || n % 4 != 0 {
        bail!("entropy must be 16-32 bytes, a multiple of 4");
    }
    if c.account >= 0x8000_0000 {
        bail!("account out of range");
    }
    if c.flags & !FLAG_USE_QSK != 0 {
        bail!("unknown flags");
    }
    if c.label.len() > LABEL_MAX || !printable(c.label.as_bytes()) {
        bail!("label must be at most 32 printable ASCII characters");
    }
    Ok(())
}

/// Seal with the given salt and nonce. `seal` picks fresh ones; this is for
/// the golden vector.
pub fn seal_with(c: &Contents, pass: &str, iterations: u32, salt: [u8; 16], nonce: [u8; 12]) -> Result<Vec<u8>> {
    if !pass_ok(pass) {
        bail!("passphrase must be 8-64 printable ASCII characters");
    }
    if !(ITERATIONS_MIN..=ITERATIONS_MAX).contains(&iterations) {
        bail!("iterations must be {ITERATIONS_MIN}..={ITERATIONS_MAX}");
    }
    check_contents(c)?;

    let mut header = Vec::with_capacity(HEADER);
    header.extend_from_slice(MAGIC);
    header.push(1);
    header.push(1);
    header.extend_from_slice(&iterations.to_le_bytes());
    header.extend_from_slice(&salt);
    header.extend_from_slice(&nonce);

    let mut payload = vec![1u8, c.entropy.len() as u8];
    payload.extend_from_slice(&c.entropy);
    payload.extend_from_slice(&c.birthday.to_le_bytes());
    payload.extend_from_slice(&c.account.to_le_bytes());
    payload.push(c.flags);
    payload.push(c.label.len() as u8);
    payload.extend_from_slice(c.label.as_bytes());

    let mut k = key(pass, iterations, &salt);
    let cipher = Aes256Gcm::new_from_slice(&k).expect("32-byte key");
    k.zeroize();
    let ct = cipher
        .encrypt(Nonce::from_slice(&nonce), Payload { msg: &payload, aad: &header })
        .map_err(|_| anyhow!("encryption failed"))?;
    payload.zeroize();
    header.extend_from_slice(&ct);
    Ok(header)
}

pub fn seal(c: &Contents, pass: &str, iterations: u32) -> Result<Vec<u8>> {
    let mut salt = [0u8; 16];
    let mut nonce = [0u8; 12];
    getrandom::fill(&mut salt).map_err(|e| anyhow!("rng: {e}"))?;
    getrandom::fill(&mut nonce).map_err(|e| anyhow!("rng: {e}"))?;
    seal_with(c, pass, iterations, salt, nonce)
}

/// The header alone: the work factor, if the file is one this reads.
pub fn peek(file: &[u8]) -> Result<u32> {
    if file.len() < HEADER + TAG || &file[..4] != MAGIC {
        bail!("not a .zkv file");
    }
    if file[4] != 1 || file[5] != 1 {
        bail!("unknown .zkv version {} / KDF {}", file[4], file[5]);
    }
    let it = u32::from_le_bytes(file[6..10].try_into().unwrap());
    if !(ITERATIONS_MIN..=ITERATIONS_MAX).contains(&it) {
        bail!("work factor {it} outside {ITERATIONS_MIN}..={ITERATIONS_MAX}");
    }
    Ok(it)
}

pub fn open(file: &[u8], pass: &str) -> Result<Contents> {
    let iterations = peek(file)?;
    let wrong = || anyhow!("wrong passphrase or damaged file");
    if !pass_ok(pass) {
        return Err(wrong());
    }
    let (header, body) = file.split_at(HEADER);
    let mut k = key(pass, iterations, &header[10..26]);
    let cipher = Aes256Gcm::new_from_slice(&k).expect("32-byte key");
    k.zeroize();
    let mut p = cipher
        .decrypt(Nonce::from_slice(&header[26..38]), Payload { msg: body, aad: header })
        .map_err(|_| wrong())?;
    let r = decode(&p);
    p.zeroize();
    r
}

fn decode(p: &[u8]) -> Result<Contents> {
    let bad = || anyhow!("malformed .zkv contents");
    if p.len() < 2 || p[0] != 1 {
        return Err(bad());
    }
    let el = p[1] as usize;
    if p.len() < 2 + el + 10 {
        return Err(bad());
    }
    let mut at = 2;
    let entropy = p[at..at + el].to_vec();
    at += el;
    let birthday = u32::from_le_bytes(p[at..at + 4].try_into().unwrap());
    at += 4;
    let account = u32::from_le_bytes(p[at..at + 4].try_into().unwrap());
    at += 4;
    let flags = p[at];
    let ll = p[at + 1] as usize;
    at += 2;
    if at + ll != p.len() {
        return Err(bad());
    }
    let label = String::from_utf8(p[at..].to_vec()).map_err(|_| bad())?;
    let c = Contents { entropy, birthday, account, flags, label };
    check_contents(&c).map_err(|_| bad())?;
    Ok(c)
}
