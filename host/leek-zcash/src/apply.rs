//! `.zsig` → signed PCZT.

use std::collections::HashSet;

use leek_device::zsig::{Entry, Pool, Zsig, ZsigError};
use pczt::roles::signer::{SpendAuthSignature, Signer};
use pczt::roles::verifier::Verifier;
use pczt::Pczt;

use crate::error::LeekError;

/// One signature the board returned.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Signature {
    /// An Orchard or Ironwood spend authorization signature (RedPallas),
    /// tagged with its pool and action index.
    SpendAuth(SpendAuthSignature),
    /// A transparent input's ECDSA signature.
    Transparent {
        /// The input index.
        index: u32,
        /// DER signature followed by the sighash type byte (always 0x01).
        sig: Vec<u8>,
        /// The compressed key it verifies under.
        pubkey: [u8; 33],
    },
}

impl Signature {
    pub(crate) fn from_entry(e: &Entry) -> Self {
        match (&e.transparent, e.pool) {
            (Some(t), _) => Signature::Transparent { index: e.index, sig: t.sig.clone(), pubkey: t.pubkey },
            (None, Pool::Ironwood) => Signature::SpendAuth(SpendAuthSignature::from_parts(
                orchard::ValuePool::Ironwood,
                e.index as usize,
                e.sig,
            )),
            (None, _) => Signature::SpendAuth(SpendAuthSignature::from_parts(
                orchard::ValuePool::Orchard,
                e.index as usize,
                e.sig,
            )),
        }
    }
}

pub(crate) fn parse_pczt(bytes: &[u8]) -> Result<Pczt, LeekError> {
    Pczt::parse(bytes).map_err(|e| match e {
        pczt::ParseError::UnknownVersion(_) => LeekError::PcztVersion,
        _ => LeekError::Parse,
    })
}

/// The shielded sighash, which (ZIP 244, v6) also commits to every
/// transparent input's amount and script.
pub(crate) fn shielded_sighash(pczt: Pczt) -> Result<[u8; 32], LeekError> {
    Ok(Signer::new(pczt).map_err(signer_error)?.shielded_sighash())
}

fn signer_error(e: pczt::roles::signer::Error) -> LeekError {
    use pczt::roles::signer::Error;
    match e {
        Error::Extract(_) => LeekError::Parse,
        Error::InvalidIndex => LeekError::SignatureInvalid,
        Error::SaplingSign(_) | Error::SaplingVerify(_) => LeekError::Unsupported(crate::Unsupported::Sapling),
        _ => LeekError::SignatureInvalid,
    }
}

/// Applies the board's `.zsig` to the wallet's own PCZT and returns it
/// signed, ready for the wallet's usual combine / prove / extract path (the
/// same place a Keystone-signed PCZT goes).
///
/// - `pczt` is the wallet's authoritative copy (full, unredacted).
/// - `sent` is the exact bytes that went to the board (the signer view of
///   `pczt`, or `pczt` itself serialized).
/// - `zsig` is the board's signatures file (version 1 or 2).
///
/// Refused unless:
/// - the `.zsig` hashes to `sent` (SHA-256) — [`LeekError::ZsigHashMismatch`];
/// - `sent` is the same transaction as `pczt` (same shielded sighash, which
///   commits to every input and output) — [`LeekError::SighashMismatch`];
/// - every signature verifies: Orchard and Ironwood against the action's
///   `rk` and the shielded sighash, transparent against the input's own
///   ZIP 244 sighash and the key its P2PKH script pays to (the upstream
///   `Signer` checks both before storing) — [`LeekError::SignatureInvalid`],
///   which also covers a wrong pool or index, a duplicate, an already signed
///   action and a sighash type other than ALL;
/// - afterwards no spend is left unsigned — [`LeekError::SignatureMissing`].
pub fn apply_zsig(pczt: &Pczt, sent: &[u8], zsig: &[u8]) -> Result<Pczt, LeekError> {
    let z = Zsig::parse(zsig).map_err(|e| match e {
        ZsigError::HashMismatch { .. } => LeekError::ZsigHashMismatch,
        _ => LeekError::ZsigMalformed,
    })?;
    z.check_hash(sent).map_err(|_| LeekError::ZsigHashMismatch)?;
    apply_entries(pczt, sent, &z.entries)
}

pub(crate) fn apply_entries(pczt: &Pczt, sent: &[u8], entries: &[Entry]) -> Result<Pczt, LeekError> {
    // Parsing resolves compacted fields, so this is the full transaction's
    // sighash even for the compact view.
    let view_sighash = shielded_sighash(parse_pczt(sent)?)?;
    if view_sighash != shielded_sighash(pczt.clone())? {
        return Err(LeekError::SighashMismatch);
    }

    let mut seen = HashSet::new();
    for e in entries {
        if !seen.insert((e.pool as u8, e.index)) {
            return Err(LeekError::SignatureInvalid);
        }
        let actions = match e.pool {
            Pool::Orchard => pczt.orchard().actions(),
            Pool::Ironwood => pczt.ironwood().actions(),
            Pool::Transparent => continue,
        };
        // The board signs only actions that lack a signature.
        match actions.get(e.index as usize) {
            Some(a) if a.spend().spend_auth_sig().is_none() => {}
            _ => return Err(LeekError::SignatureInvalid),
        }
    }

    let mut signer = Signer::new(add_transparent_preimages(pczt.clone(), entries)?).map_err(signer_error)?;
    for e in entries {
        let index = e.index as usize;
        match (e.pool, &e.transparent) {
            (Pool::Orchard | Pool::Ironwood, None) => {
                let Signature::SpendAuth(s) = Signature::from_entry(e) else { unreachable!() };
                signer.apply_orchard_spend_auth_signature(&s).map_err(|_| LeekError::SignatureInvalid)?;
            }
            (Pool::Transparent, Some(t)) => {
                if t.sighash_type() != 0x01 {
                    return Err(LeekError::SignatureInvalid);
                }
                let sig = secp256k1::ecdsa::Signature::from_der(t.der()).map_err(|_| LeekError::SignatureInvalid)?;
                signer.append_transparent_signature(index, sig).map_err(|_| LeekError::SignatureInvalid)?;
            }
            _ => return Err(LeekError::ZsigMalformed),
        }
    }
    let signed = signer.finish();
    check_complete(&signed)?;
    Ok(signed)
}

/// The pczt Signer finds a P2PKH input's key through `hash160_preimages`,
/// which wallets leave empty: add each transparent entry's key there. A key
/// that does not hash to the input's script is then not found, and its
/// signature refused.
fn add_transparent_preimages(pczt: Pczt, entries: &[Entry]) -> Result<Pczt, LeekError> {
    let ts: Vec<(usize, [u8; 33])> =
        entries.iter().filter_map(|e| e.transparent.as_ref().map(|t| (e.index as usize, t.pubkey))).collect();
    if ts.is_empty() {
        return Ok(pczt);
    }
    let n = pczt.transparent().inputs().len();
    if ts.iter().any(|(i, _)| *i >= n) {
        return Err(LeekError::SignatureInvalid);
    }
    Ok(pczt::roles::updater::Updater::new(pczt)
        .update_transparent_with(|mut u| {
            for (i, pk) in &ts {
                u.update_input_with(*i, |mut iu| {
                    iu.set_hash160_preimage(pk.to_vec());
                    Ok(())
                })?;
            }
            Ok(())
        })
        .map_err(|_| LeekError::Parse)?
        .finish())
}

/// Every Orchard-protocol action carries a spend authorization signature,
/// and every transparent input a signature.
fn check_complete(pczt: &Pczt) -> Result<(), LeekError> {
    let shielded = pczt.orchard().actions().iter().chain(pczt.ironwood().actions());
    if shielded.into_iter().any(|a| a.spend().spend_auth_sig().is_none()) {
        return Err(LeekError::SignatureMissing);
    }
    if pczt.transparent().inputs().is_empty() {
        return Ok(());
    }
    Verifier::new(pczt.clone())
        .with_transparent(|b| {
            if b.inputs().iter().any(|i| i.partial_signatures().is_empty()) {
                return Err(pczt::roles::verifier::TransparentError::Custom(LeekError::SignatureMissing));
            }
            Ok(())
        })
        .map(|_| ())
        .map_err(|e| match e {
            pczt::roles::verifier::TransparentError::Custom(l) => l,
            _ => LeekError::Parse,
        })
}
