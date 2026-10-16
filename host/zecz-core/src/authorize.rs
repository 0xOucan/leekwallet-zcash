//! Applying a `.zsig` to a PCZT: the common path for device and soft signer.

use anyhow::{anyhow, bail, Result};
use pczt::roles::prover::Prover;
use pczt::roles::signer::Signer;
use pczt::Pczt;
use rand::{rand_core::UnwrapErr, rngs::SysRng};
use zcash_protocol::consensus::BranchId;

use crate::zsig::{Pool, Zsig};

/// Applies `.zsig` signatures with the Signer role, which verifies each one
/// against its action's rk before storing it.
pub fn apply_zsig(pczt: Pczt, zsig: &Zsig) -> Result<Pczt> {
    let mut signer = Signer::new(pczt).map_err(|e| anyhow!("Signer: {e:?}"))?;
    for e in &zsig.entries {
        let sig = orchard::primitives::redpallas::Signature::<orchard::primitives::redpallas::SpendAuth>::from(e.sig);
        let idx = e.index as usize;
        match e.pool {
            Pool::Orchard => signer.apply_orchard_signature(idx, sig),
            Pool::Ironwood => signer.apply_ironwood_signature(idx, sig),
            Pool::Transparent => bail!("transparent signatures are not supported yet"),
        }
        .map_err(|err| anyhow!("{:?} signature for action {idx} rejected: {err:?}", e.pool))?;
    }
    Ok(signer.finish())
}

/// The Prover role for the Orchard-protocol bundles, using the cached proving
/// key for the circuit that governs each pool under the PCZT's consensus
/// branch. Sapling is refused: the device account has no Sapling key.
pub fn prove(pczt: Pczt) -> Result<Pczt> {
    let branch = BranchId::try_from(*pczt.global().consensus_branch_id()).map_err(|e| anyhow!("PCZT branch: {e}"))?;
    let mut prover = Prover::new(pczt);
    if prover.requires_sapling_proofs() {
        bail!("PCZT needs Sapling proofs; not supported");
    }
    let pk_for = |pool| -> Result<&'static orchard::circuit::ProvingKey> {
        let version = zcash_primitives::transaction::components::orchard::bundle_version_for_branch(branch, pool)
            .ok_or_else(|| anyhow!("branch {branch:?} has no {pool:?} pool"))?;
        eprintln!("Loading the {:?} proving key ({:?}) ...", pool, version.circuit_version());
        Ok(zcash_primitives::transaction::builder::cached_orchard_proving_key(version.circuit_version()))
    };
    if prover.requires_orchard_proof() {
        let pk = pk_for(orchard::ValuePool::Orchard)?;
        eprintln!("Proving the Orchard bundle ...");
        prover = prover.create_orchard_proof(UnwrapErr(SysRng), pk).map_err(|e| anyhow!("Orchard proof: {e:?}"))?;
    }
    if prover.requires_ironwood_proof() {
        let pk = pk_for(orchard::ValuePool::Ironwood)?;
        eprintln!("Proving the Ironwood bundle ...");
        prover = prover.create_ironwood_proof(UnwrapErr(SysRng), pk).map_err(|e| anyhow!("Ironwood proof: {e:?}"))?;
    }
    Ok(prover.finish())
}
