//! Applying a `.zsig` to a PCZT: the common path for device and soft signer.

use anyhow::{anyhow, bail, Result};
use pczt::roles::prover::Prover;
use pczt::roles::signer::Signer;
use pczt::Pczt;
use rand::{rand_core::UnwrapErr, rngs::SysRng};
use zcash_protocol::consensus::BranchId;

use crate::zsig::{Pool, Zsig};

/// Applies `.zsig` signatures with the Signer role, which verifies each
/// shielded one against its action's rk, and each transparent one against
/// its input's own ZIP 244 sighash and the key the input's script pays to,
/// before storing it.
pub fn apply_zsig(pczt: Pczt, zsig: &Zsig) -> Result<Pczt> {
    let pczt = add_transparent_preimages(pczt, zsig)?;
    let mut signer = Signer::new(pczt).map_err(|e| anyhow!("Signer: {e:?}"))?;
    for e in &zsig.entries {
        let idx = e.index as usize;
        match e.pool {
            Pool::Orchard | Pool::Ironwood => {
                let sig = orchard::primitives::redpallas::Signature::<orchard::primitives::redpallas::SpendAuth>::from(e.sig);
                if e.pool == Pool::Orchard {
                    signer.apply_orchard_signature(idx, sig)
                } else {
                    signer.apply_ironwood_signature(idx, sig)
                }
                .map_err(|err| anyhow!("{:?} signature for action {idx} rejected: {err:?}", e.pool))?;
            }
            Pool::Transparent => {
                let sig = transparent_signature(e)?;
                signer
                    .append_transparent_signature(idx, sig)
                    .map_err(|err| anyhow!("transparent signature for input {idx} rejected: {err:?}"))?;
            }
        }
    }
    Ok(signer.finish())
}

/// The DER signature of a transparent `.zsig` entry, which must be for
/// SIGHASH_ALL, the only type the device signs.
pub fn transparent_signature(e: &crate::zsig::Entry) -> Result<secp256k1::ecdsa::Signature> {
    let t = e.transparent.as_ref().ok_or_else(|| anyhow!("transparent entry without a signature"))?;
    if t.sighash_type() != 0x01 {
        bail!("transparent signature for input {} is not SIGHASH_ALL", e.index);
    }
    secp256k1::ecdsa::Signature::from_der(t.der()).map_err(|err| anyhow!("transparent signature for input {}: {err}", e.index))
}

/// The pczt Signer finds a P2PKH input's key through `hash160_preimages`,
/// which wallets do not fill in. Add each transparent entry's pubkey there
/// (an Updater step); a key that does not hash to the input's script is then
/// simply not found, and its signature is refused.
pub fn add_transparent_preimages(pczt: Pczt, zsig: &Zsig) -> Result<Pczt> {
    let ts: Vec<_> = zsig
        .entries
        .iter()
        .filter_map(|e| e.transparent.as_ref().map(|t| (e.index as usize, t.pubkey)))
        .collect();
    if ts.is_empty() {
        return Ok(pczt);
    }
    let n = pczt.transparent().inputs().len();
    if let Some((i, _)) = ts.iter().find(|(i, _)| *i >= n) {
        bail!("transparent signature for input {i}, but the PCZT has {n} inputs");
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
        .map_err(|e| anyhow!("PCZT transparent update: {e:?}"))?
        .finish())
}

/// What [`prove_with`] is doing. Proving takes seconds, so a front end
/// shows these.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ProveEvent {
    /// Loading (or building, the first time) a proving key: the pool and
    /// the circuit version, as debug names.
    LoadingKey { pool: String, circuit: String },
    /// Creating the proof for a pool's bundle ("Orchard" or "Ironwood").
    Proving { pool: &'static str },
}

/// The Prover role for the Orchard-protocol bundles, using the cached proving
/// key for the circuit that governs each pool under the PCZT's consensus
/// branch. Sapling is refused: the device account has no Sapling key.
pub fn prove(pczt: Pczt) -> Result<Pczt> {
    prove_with(pczt, |_| {})
}

/// [`prove`], reporting each step to `on_event`.
pub fn prove_with(pczt: Pczt, mut on_event: impl FnMut(ProveEvent)) -> Result<Pczt> {
    let branch = BranchId::try_from(*pczt.global().consensus_branch_id()).map_err(|e| anyhow!("PCZT branch: {e}"))?;
    let mut prover = Prover::new(pczt);
    if prover.requires_sapling_proofs() {
        bail!("PCZT needs Sapling proofs; not supported");
    }
    let pk_for = |pool: orchard::ValuePool, on_event: &mut dyn FnMut(ProveEvent)| -> Result<&'static orchard::circuit::ProvingKey> {
        let version = zcash_primitives::transaction::components::orchard::bundle_version_for_branch(branch, pool)
            .ok_or_else(|| anyhow!("branch {branch:?} has no {pool:?} pool"))?;
        on_event(ProveEvent::LoadingKey { pool: format!("{pool:?}"), circuit: format!("{:?}", version.circuit_version()) });
        Ok(zcash_primitives::transaction::builder::cached_orchard_proving_key(version.circuit_version()))
    };
    if prover.requires_orchard_proof() {
        let pk = pk_for(orchard::ValuePool::Orchard, &mut on_event)?;
        on_event(ProveEvent::Proving { pool: "Orchard" });
        prover = prover.create_orchard_proof(UnwrapErr(SysRng), pk).map_err(|e| anyhow!("Orchard proof: {e:?}"))?;
    }
    if prover.requires_ironwood_proof() {
        let pk = pk_for(orchard::ValuePool::Ironwood, &mut on_event)?;
        on_event(ProveEvent::Proving { pool: "Ironwood" });
        prover = prover.create_ironwood_proof(UnwrapErr(SysRng), pk).map_err(|e| anyhow!("Ironwood proof: {e:?}"))?;
    }
    Ok(prover.finish())
}
