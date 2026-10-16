//! The `.zsig` signatures-only file (docs/zcash/FORMATS.md).
//!
//! ```text
//! offset  size  field
//! 0       4     magic "ZSIG"
//! 4       1     version = 1
//! 5       32    SHA-256 of the exact .pczt bytes the device checked and signed
//! 37      2     count N (u16 LE)
//! 39      N*69  entries: pool (1) | action/input index (u32 LE) | signature (64)
//! ```

use std::fmt;

use sha2::{Digest, Sha256};

pub const MAGIC: &[u8; 4] = b"ZSIG";
pub const VERSION: u8 = 1;
const HEADER_LEN: usize = 4 + 1 + 32 + 2;
const ENTRY_LEN: usize = 1 + 4 + 64;

/// Signature pool codes (match `ZS_POOL_*` in zsign_rust.h).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Pool {
    /// Reserved; not produced by any signer yet.
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

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Entry {
    pub pool: Pool,
    pub index: u32,
    pub sig: [u8; 64],
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
    TrailingBytes(usize),
    TooManyEntries(usize),
    HashMismatch { expected: [u8; 32], found: [u8; 32] },
}

impl fmt::Display for ZsigError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ZsigError::Truncated => write!(f, ".zsig is truncated"),
            ZsigError::BadMagic => write!(f, ".zsig has a bad magic (expected \"ZSIG\")"),
            ZsigError::BadVersion(v) => write!(f, ".zsig version {v} is not supported (expected {VERSION})"),
            ZsigError::BadPool(p) => write!(f, ".zsig entry has unknown pool {p}"),
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
        let mut out = Vec::with_capacity(HEADER_LEN + self.entries.len() * ENTRY_LEN);
        out.extend_from_slice(MAGIC);
        out.push(VERSION);
        out.extend_from_slice(&self.pczt_sha256);
        out.extend_from_slice(&n.to_le_bytes());
        for e in &self.entries {
            out.push(e.pool as u8);
            out.extend_from_slice(&e.index.to_le_bytes());
            out.extend_from_slice(&e.sig);
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
        if bytes[4] != VERSION {
            return Err(ZsigError::BadVersion(bytes[4]));
        }
        let pczt_sha256: [u8; 32] = bytes[5..37].try_into().unwrap();
        let n = u16::from_le_bytes([bytes[37], bytes[38]]) as usize;
        let body = &bytes[HEADER_LEN..];
        let need = n * ENTRY_LEN;
        if body.len() < need {
            return Err(ZsigError::Truncated);
        }
        if body.len() > need {
            return Err(ZsigError::TrailingBytes(body.len() - need));
        }
        let entries = body
            .chunks_exact(ENTRY_LEN)
            .map(|c| {
                Ok(Entry {
                    pool: Pool::from_u8(c[0])?,
                    index: u32::from_le_bytes(c[1..5].try_into().unwrap()),
                    sig: c[5..69].try_into().unwrap(),
                })
            })
            .collect::<Result<_, ZsigError>>()?;
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
                Entry { pool: Pool::Ironwood, index: 0, sig: [0xab; 64] },
                Entry { pool: Pool::Orchard, index: 0x0102_0304, sig: core::array::from_fn(|i| i as u8) },
            ],
        );
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
        bytes[4] = 2;
        assert_eq!(Zsig::parse(&bytes), Err(ZsigError::BadVersion(2)));
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
        let (_, z) = sample();
        let bytes = z.to_bytes().unwrap();
        for len in [0, 3, 4, 20, 38, 39, 39 + 68, bytes.len() - 1] {
            assert_eq!(Zsig::parse(&bytes[..len]), Err(ZsigError::Truncated), "len {len}");
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
