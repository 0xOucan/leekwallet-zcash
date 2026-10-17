//! The `.zsig` signatures-only file (docs/zcash/FORMATS.md).
//!
//! ```text
//! offset  size  field
//! 0       4     magic "ZSIG"
//! 4       1     version: 1 if every entry is shielded, 2 if any is transparent
//! 5       32    SHA-256 of the exact .pczt bytes the device checked and signed
//! 37      2     count N (u16 LE)
//! 39      ...   entries: pool (1) | action/input index (u32 LE) | then
//!                 pool 1, 2: signature (64, RedPallas R || S)
//!                 pool 0:    L (1) | DER signature || sighash type (L) | pubkey (33)
//! ```
//!
//! A shielded-only file is always written as version 1, byte for byte what
//! version 1 readers expect.

use std::fmt;

use sha2::{Digest, Sha256};

pub const MAGIC: &[u8; 4] = b"ZSIG";
/// The version of a file with shielded signatures only.
pub const VERSION: u8 = 1;
/// The version of a file with at least one transparent signature.
pub const VERSION_TRANSPARENT: u8 = 2;
const HEADER_LEN: usize = 4 + 1 + 32 + 2;
const ENTRY_LEN: usize = 1 + 4 + 64;
/// DER (at most 72 bytes) plus the sighash type byte.
pub const TSIG_MAX: usize = 73;
const TSIG_MIN: usize = 9;

/// Signature pool codes (match `ZS_POOL_*` in zsign_rust.h).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Pool {
    Transparent = 0,
    Orchard = 1,
    Ironwood = 2,
}

impl Pool {
    fn from_u8(b: u8) -> Result<Self, ZsigError> {
        match b {
            0 => Ok(Pool::Transparent),
            1 => Ok(Pool::Orchard),
            2 => Ok(Pool::Ironwood),
            other => Err(ZsigError::BadPool(other)),
        }
    }
}

/// A transparent input's signature: DER ECDSA followed by the sighash type
/// byte, and the compressed key it verifies under.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TransparentSig {
    pub sig: Vec<u8>,
    pub pubkey: [u8; 33],
}

impl TransparentSig {
    /// The DER signature without the sighash type byte.
    pub fn der(&self) -> &[u8] {
        &self.sig[..self.sig.len().saturating_sub(1)]
    }

    /// The sighash type byte (0x01, SIGHASH_ALL, from a LeekWallet device).
    pub fn sighash_type(&self) -> u8 {
        self.sig.last().copied().unwrap_or(0)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Entry {
    pub pool: Pool,
    pub index: u32,
    /// Pools 1 and 2: RedPallas R || S. Pool 0: all zero (see `transparent`).
    pub sig: [u8; 64],
    /// Pool 0 only.
    pub transparent: Option<TransparentSig>,
}

impl Entry {
    pub fn shielded(pool: Pool, index: u32, sig: [u8; 64]) -> Self {
        Entry { pool, index, sig, transparent: None }
    }

    pub fn transparent(index: u32, sig: Vec<u8>, pubkey: [u8; 33]) -> Self {
        Entry { pool: Pool::Transparent, index, sig: [0; 64], transparent: Some(TransparentSig { sig, pubkey }) }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Zsig {
    /// SHA-256 of the exact `.pczt` bytes that were signed.
    pub pczt_sha256: [u8; 32],
    pub entries: Vec<Entry>,
}

#[derive(Debug, PartialEq, Eq)]
pub enum ZsigError {
    Truncated,
    BadMagic,
    BadVersion(u8),
    BadPool(u8),
    /// A transparent entry in a version 1 file, or a shielded entry built
    /// with transparent data (or the reverse).
    PoolMismatch(u8),
    BadSignatureLength(usize),
    TrailingBytes(usize),
    TooManyEntries(usize),
    HashMismatch { expected: [u8; 32], found: [u8; 32] },
}

impl fmt::Display for ZsigError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ZsigError::Truncated => write!(f, ".zsig is truncated"),
            ZsigError::BadMagic => write!(f, ".zsig has a bad magic (expected \"ZSIG\")"),
            ZsigError::BadVersion(v) => write!(f, ".zsig version {v} is not supported (expected 1 or 2)"),
            ZsigError::BadPool(p) => write!(f, ".zsig entry has unknown pool {p}"),
            ZsigError::PoolMismatch(p) => write!(f, ".zsig entry for pool {p} does not fit its version or its data"),
            ZsigError::BadSignatureLength(n) => write!(f, ".zsig transparent signature of {n} bytes (expected {TSIG_MIN}..{TSIG_MAX})"),
            ZsigError::TrailingBytes(n) => write!(f, ".zsig has {n} trailing bytes"),
            ZsigError::TooManyEntries(n) => write!(f, "{n} signatures do not fit a .zsig (max 65535)"),
            ZsigError::HashMismatch { expected, found } => write!(
                f,
                ".zsig was made for a different PCZT: it signs sha256 {}, but this PCZT is sha256 {}",
                hex::encode(found),
                hex::encode(expected)
            ),
        }
    }
}

impl std::error::Error for ZsigError {}

pub fn sha256(bytes: &[u8]) -> [u8; 32] {
    Sha256::digest(bytes).into()
}

impl Zsig {
    pub fn new(pczt_bytes: &[u8], entries: Vec<Entry>) -> Self {
        Zsig { pczt_sha256: sha256(pczt_bytes), entries }
    }

    pub fn to_bytes(&self) -> Result<Vec<u8>, ZsigError> {
        let n = u16::try_from(self.entries.len()).map_err(|_| ZsigError::TooManyEntries(self.entries.len()))?;
        let any_t = self.entries.iter().any(|e| e.pool == Pool::Transparent);
        let mut out = Vec::with_capacity(HEADER_LEN + self.entries.len() * ENTRY_LEN);
        out.extend_from_slice(MAGIC);
        out.push(if any_t { VERSION_TRANSPARENT } else { VERSION });
        out.extend_from_slice(&self.pczt_sha256);
        out.extend_from_slice(&n.to_le_bytes());
        for e in &self.entries {
            out.push(e.pool as u8);
            out.extend_from_slice(&e.index.to_le_bytes());
            match (e.pool, &e.transparent) {
                (Pool::Transparent, Some(t)) => {
                    if !(TSIG_MIN..=TSIG_MAX).contains(&t.sig.len()) {
                        return Err(ZsigError::BadSignatureLength(t.sig.len()));
                    }
                    out.push(t.sig.len() as u8);
                    out.extend_from_slice(&t.sig);
                    out.extend_from_slice(&t.pubkey);
                }
                (Pool::Orchard | Pool::Ironwood, None) => out.extend_from_slice(&e.sig),
                _ => return Err(ZsigError::PoolMismatch(e.pool as u8)),
            }
        }
        Ok(out)
    }

    pub fn parse(bytes: &[u8]) -> Result<Self, ZsigError> {
        if bytes.len() < 4 {
            return Err(ZsigError::Truncated);
        }
        if &bytes[..4] != MAGIC {
            return Err(ZsigError::BadMagic);
        }
        if bytes.len() < HEADER_LEN {
            return Err(ZsigError::Truncated);
        }
        let version = bytes[4];
        if version != VERSION && version != VERSION_TRANSPARENT {
            return Err(ZsigError::BadVersion(version));
        }
        let pczt_sha256: [u8; 32] = bytes[5..37].try_into().unwrap();
        let n = u16::from_le_bytes([bytes[37], bytes[38]]) as usize;
        let mut body = &bytes[HEADER_LEN..];
        let mut entries = Vec::with_capacity(n);
        let take = |body: &mut &[u8], k: usize| -> Result<Vec<u8>, ZsigError> {
            if body.len() < k {
                return Err(ZsigError::Truncated);
            }
            let (a, b) = body.split_at(k);
            *body = b;
            Ok(a.to_vec())
        };
        for _ in 0..n {
            let head = take(&mut body, 5)?;
            let pool = Pool::from_u8(head[0])?;
            let index = u32::from_le_bytes(head[1..5].try_into().unwrap());
            if pool == Pool::Transparent {
                if version == VERSION {
                    return Err(ZsigError::PoolMismatch(0));
                }
                let l = take(&mut body, 1)?[0] as usize;
                if !(TSIG_MIN..=TSIG_MAX).contains(&l) {
                    return Err(ZsigError::BadSignatureLength(l));
                }
                let sig = take(&mut body, l)?;
                let pubkey: [u8; 33] = take(&mut body, 33)?.try_into().unwrap();
                entries.push(Entry::transparent(index, sig, pubkey));
            } else {
                let sig: [u8; 64] = take(&mut body, 64)?.try_into().unwrap();
                entries.push(Entry::shielded(pool, index, sig));
            }
        }
        if !body.is_empty() {
            return Err(ZsigError::TrailingBytes(body.len()));
        }
        Ok(Zsig { pczt_sha256, entries })
    }

    /// Parses a `.zsig` and refuses it unless it signs exactly `pczt_bytes`.
    pub fn parse_for(bytes: &[u8], pczt_bytes: &[u8]) -> Result<Self, ZsigError> {
        let z = Self::parse(bytes)?;
        z.check_hash(pczt_bytes)?;
        Ok(z)
    }

    pub fn check_hash(&self, pczt_bytes: &[u8]) -> Result<(), ZsigError> {
        let expected = sha256(pczt_bytes);
        if expected != self.pczt_sha256 {
            return Err(ZsigError::HashMismatch { expected, found: self.pczt_sha256 });
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> (Vec<u8>, Zsig) {
        let pczt = b"PCZT\x02\x00\x00\x00not really a pczt".to_vec();
        let z = Zsig::new(
            &pczt,
            vec![
                Entry::shielded(Pool::Ironwood, 0, [0xab; 64]),
                Entry::shielded(Pool::Orchard, 0x0102_0304, core::array::from_fn(|i| i as u8)),
            ],
        );
        (pczt, z)
    }

    fn sample_t() -> (Vec<u8>, Zsig) {
        let (pczt, mut z) = sample();
        let mut der = vec![0x30, 0x44, 0x02, 0x20];
        der.extend([0x11; 32]);
        der.extend([0x02, 0x20]);
        der.extend([0x22; 32]);
        der.push(0x01);
        z.entries.push(Entry::transparent(1, der, [0x02; 33]));
        (pczt, z)
    }

    #[test]
    fn round_trip() {
        let (pczt, z) = sample();
        let bytes = z.to_bytes().unwrap();
        assert_eq!(bytes.len(), 39 + 2 * 69);
        assert_eq!(&bytes[..5], b"ZSIG\x01");
        assert_eq!(&bytes[37..39], &[2, 0]);
        // First entry: pool 2, index 0 LE, then the signature.
        assert_eq!(bytes[39], 2);
        assert_eq!(&bytes[40..44], &[0, 0, 0, 0]);
        // Second entry index is little-endian.
        assert_eq!(&bytes[39 + 69 + 1..39 + 69 + 5], &[4, 3, 2, 1]);
        assert_eq!(Zsig::parse_for(&bytes, &pczt).unwrap(), z);
    }

    #[test]
    fn transparent_round_trip() {
        let (pczt, z) = sample_t();
        let bytes = z.to_bytes().unwrap();
        assert_eq!(bytes[4], VERSION_TRANSPARENT);
        assert_eq!(bytes.len(), 39 + 2 * 69 + 1 + 4 + 1 + 71 + 33);
        let t = 39 + 2 * 69;
        assert_eq!(&bytes[t..t + 6], &[0, 1, 0, 0, 0, 71]);
        let back = Zsig::parse_for(&bytes, &pczt).unwrap();
        assert_eq!(back, z);
        let ts = back.entries[2].transparent.as_ref().unwrap();
        assert_eq!(ts.sighash_type(), 1);
        assert_eq!(ts.der().len(), 70);
    }

    #[test]
    fn transparent_needs_version_2() {
        let (_, z) = sample_t();
        let mut bytes = z.to_bytes().unwrap();
        bytes[4] = 1;
        assert_eq!(Zsig::parse(&bytes), Err(ZsigError::PoolMismatch(0)));
    }

    #[test]
    fn transparent_bad_length() {
        let (_, z) = sample_t();
        let mut bytes = z.to_bytes().unwrap();
        bytes[39 + 2 * 69 + 5] = 74;
        assert_eq!(Zsig::parse(&bytes), Err(ZsigError::BadSignatureLength(74)));
        let mut bad = z.clone();
        bad.entries[2].transparent.as_mut().unwrap().sig.truncate(3);
        assert_eq!(bad.to_bytes(), Err(ZsigError::BadSignatureLength(3)));
    }

    #[test]
    fn version_2_reads_shielded_only_too() {
        let (_, z) = sample();
        let mut bytes = z.to_bytes().unwrap();
        bytes[4] = 2;
        assert_eq!(Zsig::parse(&bytes).unwrap(), z);
    }

    #[test]
    fn empty_round_trip() {
        let z = Zsig::new(b"x", vec![]);
        let bytes = z.to_bytes().unwrap();
        assert_eq!(bytes.len(), 39);
        assert_eq!(Zsig::parse(&bytes).unwrap(), z);
    }

    #[test]
    fn bad_magic() {
        let (_, z) = sample();
        let mut bytes = z.to_bytes().unwrap();
        bytes[0] = b'X';
        assert_eq!(Zsig::parse(&bytes), Err(ZsigError::BadMagic));
        assert_eq!(Zsig::parse(b"PCZT\x01"), Err(ZsigError::BadMagic));
    }

    #[test]
    fn bad_version() {
        let (_, z) = sample();
        let mut bytes = z.to_bytes().unwrap();
        bytes[4] = 3;
        assert_eq!(Zsig::parse(&bytes), Err(ZsigError::BadVersion(3)));
    }

    #[test]
    fn bad_pool() {
        let (_, z) = sample();
        let mut bytes = z.to_bytes().unwrap();
        bytes[39] = 7;
        assert_eq!(Zsig::parse(&bytes), Err(ZsigError::BadPool(7)));
    }

    #[test]
    fn hash_mismatch() {
        let (mut pczt, z) = sample();
        let bytes = z.to_bytes().unwrap();
        pczt.push(0);
        assert!(matches!(Zsig::parse_for(&bytes, &pczt), Err(ZsigError::HashMismatch { .. })));
    }

    #[test]
    fn truncated() {
        for (_, z) in [sample(), sample_t()] {
            let bytes = z.to_bytes().unwrap();
            for len in [0, 3, 4, 20, 38, 39, 39 + 68, bytes.len() - 34, bytes.len() - 1] {
                assert_eq!(Zsig::parse(&bytes[..len]), Err(ZsigError::Truncated), "len {len}");
            }
        }
    }

    #[test]
    fn trailing_bytes() {
        let (_, z) = sample();
        let mut bytes = z.to_bytes().unwrap();
        bytes.push(0);
        assert_eq!(Zsig::parse(&bytes), Err(ZsigError::TrailingBytes(1)));
    }
}
