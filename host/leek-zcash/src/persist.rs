//! Keeping a signed result across a crash, and checking what was mined.
//!
//! A wallet that loses a signed transaction between signing and broadcast
//! cannot tell whether it went out; one that re-signs may build a second
//! spend of different notes. So: persist [`SignResult::to_bytes`] (it
//! carries the txid) before broadcasting, broadcast idempotently (the same
//! bytes again, never a re-signed transaction, until the txid is mined or
//! expired), and after mining fetch the transaction and compare it with
//! [`check_mined`].

use pczt::Pczt;
use zcash_primitives::transaction::txid::{to_txid, TxIdDigester};
use zcash_primitives::transaction::{Transaction, TxId};
use zcash_protocol::consensus::BranchId;

use leek_device::zsig::{sha256, Zsig, ZsigError};

use crate::apply::{parse_pczt, shielded_sighash, Signature};
use crate::error::LeekError;
use crate::sign::SignResult;

const MAGIC: &[u8; 4] = b"LKSR";
const VERSION: u8 = 1;
const HEADER_LEN: usize = 4 + 1 + 32 + 32;

/// The ZIP 244 txid of a PCZT's effects: final once the inputs and outputs
/// are, before proofs and signatures.
pub(crate) fn pczt_txid(pczt: &Pczt) -> Result<[u8; 32], LeekError> {
    let data = pczt.clone().into_effects().map_err(|_| LeekError::Parse)?;
    let digests = data.digest(TxIdDigester);
    // None only for a version ZIP 244 has no txid for; the board signs v5 and
    // v6, and a PCZT is v5 or later by construction.
    let txid = to_txid(data.version(), data.consensus_branch_id(), &digests).ok_or(LeekError::Parse)?;
    Ok(*txid.as_ref())
}

fn put(out: &mut Vec<u8>, bytes: &[u8]) -> Result<(), LeekError> {
    let n = u32::try_from(bytes.len()).map_err(|_| LeekError::TooLarge)?;
    out.extend_from_slice(&n.to_le_bytes());
    out.extend_from_slice(bytes);
    Ok(())
}

fn take<'a>(body: &mut &'a [u8]) -> Result<&'a [u8], LeekError> {
    if body.len() < 4 {
        return Err(LeekError::Parse);
    }
    let n = u32::from_le_bytes(body[..4].try_into().unwrap()) as usize;
    let rest = &body[4..];
    if rest.len() < n {
        return Err(LeekError::Parse);
    }
    let (a, b) = rest.split_at(n);
    *body = b;
    Ok(a)
}

impl SignResult {
    /// The txid as explorers, RPCs and lightwalletd show it (byte-reversed
    /// hex).
    pub fn txid_hex(&self) -> String {
        TxId::from_bytes(self.txid).to_string()
    }

    /// The result as bytes, to persist before broadcasting:
    ///
    /// ```text
    /// "LKSR" | version 1 | sighash (32) | txid (32, internal order)
    ///        | u32 LE length | signed PCZT | u32 LE length | .zsig
    ///        | SHA-256 of everything before it (32)
    /// ```
    ///
    /// The signatures are in the `.zsig`, so they are not stored twice.
    pub fn to_bytes(&self) -> Result<Vec<u8>, LeekError> {
        let pczt = self.signed_pczt.clone().serialize().map_err(|_| LeekError::Parse)?;
        let mut out = Vec::with_capacity(HEADER_LEN + 8 + pczt.len() + self.zsig.len());
        out.extend_from_slice(MAGIC);
        out.push(VERSION);
        out.extend_from_slice(&self.sighash);
        out.extend_from_slice(&self.txid);
        put(&mut out, &pczt)?;
        put(&mut out, &self.zsig)?;
        let check = sha256(&out);
        out.extend_from_slice(&check);
        Ok(out)
    }

    /// Reads [`to_bytes`](Self::to_bytes) back. The checksum must match, and
    /// the txid and the sighash are recomputed from the stored PCZT and must
    /// match the stored ones, so a damaged record is [`LeekError::Parse`]
    /// rather than a wrong txid. The
    /// signatures were verified when the result was made; this does not
    /// verify them again (the wallet's extract step does).
    pub fn from_bytes(bytes: &[u8]) -> Result<Self, LeekError> {
        if bytes.len() < HEADER_LEN + 32 || &bytes[..4] != MAGIC {
            return Err(LeekError::Parse);
        }
        if bytes[4] != VERSION {
            return Err(LeekError::PcztVersion);
        }
        let (bytes, check) = bytes.split_at(bytes.len() - 32);
        if sha256(bytes) != check {
            return Err(LeekError::Parse);
        }
        let sighash: [u8; 32] = bytes[5..37].try_into().unwrap();
        let txid: [u8; 32] = bytes[37..69].try_into().unwrap();
        let mut body = &bytes[HEADER_LEN..];
        let signed_pczt = parse_pczt(take(&mut body)?)?;
        let zsig = take(&mut body)?.to_vec();
        if !body.is_empty() {
            return Err(LeekError::Parse);
        }
        let z = Zsig::parse(&zsig).map_err(|e: ZsigError| match e {
            ZsigError::HashMismatch { .. } => LeekError::ZsigHashMismatch,
            _ => LeekError::ZsigMalformed,
        })?;
        if pczt_txid(&signed_pczt)? != txid || shielded_sighash(signed_pczt.clone())? != sighash {
            return Err(LeekError::Parse);
        }
        Ok(SignResult {
            signatures: z.entries.iter().map(Signature::from_entry).collect(),
            signed_pczt,
            sighash,
            zsig,
            txid,
        })
    }
}

/// How a mined transaction compares with the one the wallet extracted.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MinedCheck {
    /// Same txid and same authorizing data (proofs, signatures): the
    /// wallet's own transaction was mined.
    Identical,
    /// Same txid, other authorizing data: the same inputs and outputs, but
    /// proved or signed again (for example a second broadcast after
    /// re-proving). The payment is the one the user approved.
    SameEffectsOtherAuth,
    /// A different transaction: the wallet's was not the one mined.
    Different,
}

fn read_tx(bytes: &[u8]) -> Result<Transaction, LeekError> {
    if bytes.len() < 12 {
        return Err(LeekError::Parse);
    }
    // v5 and later carry their consensus branch in the header.
    let version = u32::from_le_bytes(bytes[0..4].try_into().unwrap()) & 0x7FFF_FFFF;
    if version < 5 {
        return Err(LeekError::Unsupported(crate::Unsupported::Version));
    }
    let branch = BranchId::try_from(u32::from_le_bytes(bytes[8..12].try_into().unwrap()))
        .map_err(|_| LeekError::Unsupported(crate::Unsupported::Branch))?;
    let tx = Transaction::read(bytes, branch).map_err(|_| LeekError::Parse)?;
    Ok(tx)
}

/// The txid of a serialized transaction (v5 or later), in internal order.
pub fn tx_txid(tx: &[u8]) -> Result<[u8; 32], LeekError> {
    Ok(*read_tx(tx)?.txid().as_ref())
}

/// Compares the transaction the wallet extracted (`signed_tx`, the bytes it
/// broadcast) with the one fetched from the chain after mining
/// (`mined_tx`): txid first (the effects), then the authorizing data
/// commitment (ZIP 244's `auth_digest`: proofs and signatures).
pub fn check_mined(signed_tx: &[u8], mined_tx: &[u8]) -> Result<MinedCheck, LeekError> {
    let ours = read_tx(signed_tx)?;
    let mined = read_tx(mined_tx)?;
    if ours.txid() != mined.txid() {
        return Ok(MinedCheck::Different);
    }
    if ours.auth_commitment().as_bytes() == mined.auth_commitment().as_bytes() {
        Ok(MinedCheck::Identical)
    } else {
        Ok(MinedCheck::SameEffectsOtherAuth)
    }
}
