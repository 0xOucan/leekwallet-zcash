//! DEV ONLY: a reference software signer that returns exactly what the device
//! returns, a signatures-only `.zsig`.
//!
//! It runs upstream `pczt`'s Signer role with keys from a mnemonic, signs every
//! Orchard and Ironwood spend that still lacks a spend authorization
//! signature, and extracts the new signatures with
//! `extract_orchard_spend_auth_signatures`. It does none of the device's
//! semantic checks (outputs, user addresses, memos); it is for exercising the
//! host flow, never for real funds.

use anyhow::{anyhow, bail, Result};
use pczt::roles::signer::{extract_orchard_spend_auth_signatures, Signer};
use pczt::Pczt;
use rand::{rand_core::UnwrapErr, rngs::SysRng};

use crate::keys::DevAccount;
use crate::zsig::{Entry, Pool, Zsig};

/// Signs the PCZT in `pczt_bytes` (the exact file the device would receive)
/// and returns the `.zsig` that commits to those bytes.
pub fn sign(pczt_bytes: &[u8], account: &DevAccount, coin_type: u32) -> Result<Zsig> {
    let pczt = Pczt::parse(pczt_bytes).map_err(|e| anyhow!("PCZT parse error: {e:?}"))?;

    if *pczt.global().coin_type() != coin_type {
        bail!("PCZT is for coin type {}, signer is for {coin_type}", pczt.global().coin_type());
    }
    if !pczt.sapling().spends().is_empty() {
        bail!("PCZT has Sapling spends; this signer (like the device) does not sign Sapling");
    }
    if !pczt.transparent().inputs().is_empty() {
        bail!("PCZT has transparent inputs; this signer (like the device) does not sign them yet");
    }

    // Spends we must sign: no signature yet. Dummy keys must never be present.
    let todo = |bundle: &pczt::orchard::Bundle| -> Result<Vec<usize>> {
        let mut v = Vec::new();
        for (i, a) in bundle.actions().iter().enumerate() {
            if a.spend().dummy_sk().is_some() {
                bail!("PCZT carries a dummy spending key (action {i}); refusing");
            }
            if a.spend().spend_auth_sig().is_none() {
                v.push(i);
            }
        }
        Ok(v)
    };
    let orchard_todo = todo(pczt.orchard())?;
    let ironwood_todo = todo(pczt.ironwood())?;
    if orchard_todo.is_empty() && ironwood_todo.is_empty() {
        bail!("nothing to sign: every Orchard/Ironwood spend is already signed");
    }

    let ask = account.ask();
    let mut signer = Signer::new(pczt).map_err(|e| anyhow!("Signer: {e:?}"))?;
    for &i in &orchard_todo {
        signer
            .sign_orchard(UnwrapErr(SysRng), i, &ask)
            .map_err(|e| anyhow!("signing Orchard action {i}: {e:?}"))?;
    }
    for &i in &ironwood_todo {
        signer
            .sign_ironwood(UnwrapErr(SysRng), i, &ask)
            .map_err(|e| anyhow!("signing Ironwood action {i}: {e:?}"))?;
    }
    let signed = signer.finish();

    // Only return the signatures this signer made, like the device.
    let entries = extract_orchard_spend_auth_signatures(&signed)
        .into_iter()
        .filter_map(|s| {
            let (pool, todo) = match s.value_pool() {
                orchard::ValuePool::Orchard => (Pool::Orchard, &orchard_todo),
                orchard::ValuePool::Ironwood => (Pool::Ironwood, &ironwood_todo),
            };
            todo.contains(&s.action_index()).then(|| Entry {
                pool,
                index: s.action_index() as u32,
                sig: *s.signature(),
            })
        })
        .collect();

    Ok(Zsig::new(pczt_bytes, entries))
}

/// Applies `.zsig` signatures with the Signer role, which verifies each one
/// against its action's rk before storing it.
pub fn apply(pczt: Pczt, zsig: &Zsig) -> Result<Pczt> {
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
