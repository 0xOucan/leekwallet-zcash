//! The rest of the companion's work, on the same librustzcash rev as zecz-core:
//! sign (dev soft signer, standing in for the board), prove and extract a real
//! v6/NU6.3 Ironwood PCZT, and trial-decrypt compact blocks the way
//! `zcash_client_backend::sync` does.
//!
//! What is NOT here is the wallet database: `zcash_client_sqlite` does not
//! build for wasm32-unknown-unknown at this rev, and `zcash_client_memory` is
//! pre-Ironwood (see README / docs/zcash/CHROME-ENGINE.md). Without a
//! `WalletRead + WalletWrite` impl there is no `sync::run`, `propose_transfer`
//! or `create_pczt_from_proposal`, so the spike exercises the pieces around
//! them.

use std::convert::Infallible;

use orchard::circuit::{ProvingKey, VerifyingKey};
use pczt::roles::{prover::Prover, signer::Signer, tx_extractor::TransactionExtractor};
use pczt::Pczt;
use rand::{rand_core::UnwrapErr, rngs::SysRng};
use zcash_client_backend::proto::compact_formats::CompactBlock;
use zcash_client_backend::scanning::{scan_block, ScanningKeys, SpendIdentifiers};
use zcash_keys::keys::{UnifiedFullViewingKey, UnifiedSpendingKey};
use zcash_protocol::consensus::{Network, NetworkConstants};

pub const NETWORK: Network = Network::TestNetwork;

/// Timings (ms) from a caller-supplied clock, so native and wasm share the code.
#[derive(Debug, Default)]
pub struct FlowTimes {
    pub sign_ms: f64,
    pub prove_ms: f64,
    pub extract_ms: f64,
    pub txid: String,
    pub tx_len: usize,
    pub actions: usize,
}

fn seed_from_mnemonic(mnemonic: &str) -> Result<[u8; 64], String> {
    let m = bip39::Mnemonic::parse_normalized(mnemonic).map_err(|e| format!("mnemonic: {e}"))?;
    Ok(m.to_seed_normalized(""))
}

/// The UFVK of account 0 of `mnemonic` on testnet.
pub fn ufvk_for(mnemonic: &str) -> Result<UnifiedFullViewingKey, String> {
    let seed = seed_from_mnemonic(mnemonic)?;
    let usk = UnifiedSpendingKey::from_seed(&NETWORK, &seed, zip32::AccountId::ZERO)
        .map_err(|e| format!("usk: {e:?}"))?;
    Ok(usk.to_unified_full_viewing_key())
}

/// Sign every unsigned Orchard/Ironwood spend (what the board does), then
/// prove and extract (what the companion does after the board returns).
pub fn sign_prove_extract(
    pczt_bytes: &[u8],
    mnemonic: &str,
    pk: &ProvingKey,
    vk: &VerifyingKey,
    now: impl Fn() -> f64,
) -> Result<FlowTimes, String> {
    let mut t = FlowTimes::default();
    let pczt = Pczt::parse(pczt_bytes).map_err(|e| format!("parse: {e:?}"))?;

    let t0 = now();
    let seed = seed_from_mnemonic(mnemonic)?;
    let sk = orchard::keys::SpendingKey::from_zip32_seed(&seed, NETWORK.coin_type(), zip32::AccountId::ZERO)
        .map_err(|e| format!("sk: {e:?}"))?;
    let ask = orchard::keys::SpendAuthorizingKey::from(&sk);
    let unsigned = |b: &pczt::orchard::Bundle| -> Vec<usize> {
        b.actions()
            .iter()
            .enumerate()
            .filter(|(_, a)| a.spend().spend_auth_sig().is_none() && a.spend().dummy_sk().is_none())
            .map(|(i, _)| i)
            .collect()
    };
    let o = unsigned(pczt.orchard());
    let iw = unsigned(pczt.ironwood());
    t.actions = pczt.orchard().actions().len() + pczt.ironwood().actions().len();
    let mut signer = Signer::new(pczt).map_err(|e| format!("signer: {e:?}"))?;
    for i in o {
        signer.sign_orchard(UnwrapErr(SysRng), i, &ask).map_err(|e| format!("sign orchard {i}: {e:?}"))?;
    }
    for i in iw {
        signer.sign_ironwood(UnwrapErr(SysRng), i, &ask).map_err(|e| format!("sign ironwood {i}: {e:?}"))?;
    }
    let signed = signer.finish();
    t.sign_ms = now() - t0;

    let t0 = now();
    let mut prover = Prover::new(signed);
    if prover.requires_sapling_proofs() {
        return Err("PCZT needs Sapling proofs".into());
    }
    if prover.requires_orchard_proof() {
        prover = prover.create_orchard_proof(UnwrapErr(SysRng), pk).map_err(|e| format!("orchard proof: {e:?}"))?;
    }
    if prover.requires_ironwood_proof() {
        prover = prover.create_ironwood_proof(UnwrapErr(SysRng), pk).map_err(|e| format!("ironwood proof: {e:?}"))?;
    }
    let proven = prover.finish();
    t.prove_ms = now() - t0;

    let t0 = now();
    let tx = TransactionExtractor::new(proven)
        .with_orchard(vk)
        .extract(UnwrapErr(SysRng))
        .map_err(|e| format!("extract: {e:?}"))?;
    let mut raw = Vec::new();
    tx.write(&mut raw).map_err(|e| format!("write: {e}"))?;
    t.extract_ms = now() - t0;
    t.txid = tx.txid().to_string();
    t.tx_len = raw.len();
    Ok(t)
}

#[derive(Debug, Default)]
pub struct ScanStats {
    pub blocks: usize,
    pub actions: usize,
    pub sapling_outputs: usize,
    pub found: usize,
}

/// Trial-decrypts `blocks` (consecutive, as from GetBlockRange) for `ufvk`,
/// as `sync::run` does per batch.
pub fn scan(blocks: Vec<CompactBlock>, ufvk: &UnifiedFullViewingKey) -> Result<ScanStats, String> {
    let keys = ScanningKeys::from_account_ufvks([(0u32, ufvk.clone())]);
    let mut spends = SpendIdentifiers::<u32>::empty();
    let mut prior = None;
    let mut s = ScanStats::default();
    for block in blocks {
        s.blocks += 1;
        for tx in &block.vtx {
            s.actions += tx.actions.len() + tx.ironwood_actions.len();
            s.sapling_outputs += tx.outputs.len();
        }
        let scanned = scan_block(&NETWORK, block, &keys, &spends, prior.as_ref(), |_| {
            Ok::<_, Infallible>(None)
        })
        .map_err(|e| format!("scan: {e}"))?;
        s.found += scanned.transactions().len();
        spends.update_with(&scanned);
        prior = Some(scanned.to_block_metadata());
    }
    Ok(s)
}
