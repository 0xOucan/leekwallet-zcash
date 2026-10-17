//! DEV ONLY: a reference software signer that returns exactly what the device
//! returns, a signatures-only `.zsig`.
//!
//! It runs upstream `pczt`'s Signer role with keys from a mnemonic, signs every
//! Orchard and Ironwood spend that still lacks a spend authorization
//! signature, and extracts the new signatures with
//! `extract_orchard_spend_auth_signatures`; transparent inputs at this
//! account's BIP 44 paths are signed over their ZIP 244 sighash (RFC 6979
//! ECDSA, as the device). It does none of the device's
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
    // Transparent inputs: the key at each input's own BIP 44 path, which must
    // be this account's (the device checks much more; see FORMATS.md).
    let t_keys = transparent_keys(&pczt, account, coin_type)?;

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
    if orchard_todo.is_empty() && ironwood_todo.is_empty() && t_keys.is_empty() {
        bail!("nothing to sign: every spend is already signed");
    }

    let ask = account.ask();
    let mut signer = Signer::new(pczt).map_err(|e| anyhow!("Signer: {e:?}"))?;
    let mut t_entries = Vec::new();
    for (i, sk) in t_keys.iter().enumerate() {
        let sighash = signer.transparent_sighash(i).map_err(|e| anyhow!("transparent input {i}: {e:?}"))?;
        let sig = secp256k1::ecdsa::sign(secp256k1::Message::from_digest(sighash), sk);
        let mut der = sig.serialize_der().to_vec();
        der.push(0x01); // SIGHASH_ALL
        t_entries.push(Entry::transparent(i as u32, der, sk.public_key().serialize()));
    }
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
            todo.contains(&s.action_index()).then(|| Entry::shielded(pool, s.action_index() as u32, *s.signature()))
        })
        .chain(t_entries)
        .collect();

    Ok(Zsig::new(pczt_bytes, entries))
}

/// The secret key of every transparent input, from its BIP 44 derivation,
/// which must name this seed and account; the key's P2PKH script must be the
/// input's.
fn transparent_keys(pczt: &Pczt, account: &DevAccount, coin_type: u32) -> Result<Vec<secp256k1::SecretKey>> {
    use zcash_protocol::consensus::{MainNetwork, TestNetwork};
    use zcash_transparent::address::TransparentAddress;
    use zcash_transparent::keys::{AccountPrivKey, NonHardenedChildIndex, TransparentKeyScope};

    if pczt.transparent().inputs().is_empty() {
        return Ok(Vec::new());
    }
    let seed = account.seed_bytes();
    let key = if coin_type == 133 {
        AccountPrivKey::from_seed(&MainNetwork, seed, account.account)
    } else {
        AccountPrivKey::from_seed(&TestNetwork, seed, account.account)
    }
    .map_err(|e| anyhow!("transparent account key: {e:?}"))?;
    let fp = account.seed_fingerprint.to_bytes();
    let acct = u32::from(account.account);

    let mut keys = Vec::new();
    let mut err = None;
    let _ = pczt::roles::verifier::Verifier::new(pczt.clone()).with_transparent::<(), _>(|b| {
        for (i, input) in b.inputs().iter().enumerate() {
            let found = input.bip32_derivation().iter().find_map(|(pk, d)| {
                let path: Vec<u32> = d.derivation_path().iter().map(|c| c.0).collect();
                match path[..] {
                    [p, c, a, s, idx]
                        if d.seed_fingerprint() == &fp
                            && p == 44 | 0x8000_0000
                            && c == coin_type | 0x8000_0000
                            && a == acct | 0x8000_0000 =>
                    {
                        let sk = key
                            .derive_secret_key(TransparentKeyScope::custom(s)?, NonHardenedChildIndex::from_index(idx)?)
                            .ok()?;
                        let TransparentAddress::PublicKeyHash(h) = TransparentAddress::from_pubkey(&sk.public_key()) else {
                            return None;
                        };
                        let script = [&[0x76, 0xa9, 0x14][..], &h, &[0x88, 0xac]].concat();
                        (sk.public_key().serialize() == *pk && script == pczt.transparent().inputs()[i].script_pubkey()[..])
                            .then_some(sk)
                    }
                    _ => None,
                }
            });
            match found {
                Some(sk) => keys.push(sk),
                None => {
                    err = Some(anyhow!("transparent input {i} is not this account's (no matching BIP 44 derivation)"));
                    break;
                }
            }
        }
        Ok(())
    });
    if let Some(e) = err {
        return Err(e);
    }
    if keys.len() != pczt.transparent().inputs().len() {
        bail!("transparent inputs do not parse");
    }
    Ok(keys)
}
