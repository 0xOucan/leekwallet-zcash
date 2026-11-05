//! Filling in the derivations some wallets leave out.

use pczt::roles::updater::Updater;
use pczt::Pczt;
use zcash_address::unified::{self, Encoding};

use crate::account::{AccountMeta, Network};
use crate::error::LeekError;
use crate::preflight::{decode_ufvk, derive_t_pubkey, p2pkh_hash, pays_to, spend_is_ours, H};

/// How far [`prepare_for_signer`] searches each transparent scope for the
/// key of an input that names no path at all.
pub const TRANSPARENT_SEARCH_LIMIT: u32 = 1000;

/// Fills in the key derivations the board needs and some wallets leave out,
/// for this account only, and returns the updated PCZT.
///
/// - Each unsigned Orchard or Ironwood spend that provably belongs to the
///   UFVK's Orchard key (its nullifier and `rk` check out) and has no ZIP 32
///   derivation, or one with a zero seed fingerprint, gets
///   `m/32'/coin'/account'` under `seed_fp`.
/// - Each P2PKH transparent input gets a full BIP 32 derivation
///   (`m/44'/coin'/account'/scope/index` under `seed_fp`): an entry with a
///   zero fingerprint (zkool2 writes one) keeps its path once the UFVK's key
///   at that path is checked to be the input's; an input with no entry is
///   matched against the account's keys, scopes 0 to 2, indices below
///   [`TRANSPARENT_SEARCH_LIMIT`].
///
/// Spends and inputs that are not this account's are left as they are, for
/// [`preflight`](crate::preflight) and the board to refuse. The derivation
/// fields do not enter any sighash, so the board's signatures over the
/// prepared PCZT apply to the original too. Then send the signer view
/// ([`signer_view_bytes`](crate::signer_view_bytes)), as
/// [`sign_pczt`](crate::sign_pczt) does.
pub fn prepare_for_signer(pczt: Pczt, ufvk: &str, seed_fp: &[u8; 32], account: u32) -> Result<Pczt, LeekError> {
    let (net, _, _) = unified::Ufvk::decode(ufvk).map_err(|_| LeekError::Parse)?;
    let network = Network::from_type(net)?;
    if Network::from_coin_type(*pczt.global().coin_type()) != Some(network) {
        return Err(LeekError::WrongNetwork);
    }
    let keys = decode_ufvk(ufvk, network)?;
    let meta = AccountMeta { seed_fingerprint: *seed_fp, index: account, network, ufvk: None };
    let zip32_path = vec![32 | H, network.coin_type() | H, account | H];

    let has_orchard = !pczt.orchard().actions().is_empty();
    let has_ironwood = !pczt.ironwood().actions().is_empty();
    let mut u = Updater::new(pczt);
    if let Some(fvk) = &keys.orchard {
        let fill = |mut up: orchard::pczt::Updater<'_>| -> Result<(), orchard::pczt::UpdaterError> {
            let todo: Vec<usize> = up
                .bundle()
                .actions()
                .iter()
                .enumerate()
                .filter(|(_, a)| {
                    let s = a.spend();
                    s.spend_auth_sig().is_none()
                        && s.zip32_derivation().as_ref().is_none_or(|d| *d.seed_fingerprint() == [0; 32])
                        && spend_is_ours(s, fvk) == Some(true)
                })
                .map(|(i, _)| i)
                .collect();
            for i in todo {
                let d = orchard::pczt::Zip32Derivation::parse(*seed_fp, zip32_path.clone())
                    .expect("a hardened ZIP 32 path");
                up.update_action_with(i, |mut a| {
                    a.set_spend_zip32_derivation(d);
                    Ok(())
                })?;
            }
            Ok(())
        };
        if has_orchard {
            u = u.update_orchard_with(fill).map_err(|_| LeekError::Parse)?;
        }
        if has_ironwood {
            u = u.update_ironwood_with(fill).map_err(|_| LeekError::Parse)?;
        }
    }
    let pczt = u.finish();

    let Some(tpub) = keys.transparent else { return Ok(pczt) };
    if pczt.transparent().inputs().is_empty() {
        return Ok(pczt);
    }
    Updater::new(pczt)
        .update_transparent_with(|mut up| {
            let mut found = Vec::new();
            for (i, input) in up.bundle().inputs().iter().enumerate() {
                let Some(hash) = p2pkh_hash(input) else { continue };
                let ours = input.bip32_derivation().iter().any(|(pk, d)| {
                    *d.seed_fingerprint() == *seed_fp && pays_to(pk, &hash)
                });
                if ours {
                    continue;
                }
                // A zero-fingerprint entry whose path the UFVK confirms.
                let from_entry = input.bip32_derivation().iter().find_map(|(pk, d)| {
                    if *d.seed_fingerprint() != [0; 32] {
                        return None;
                    }
                    let path: Vec<u32> = d.derivation_path().iter().map(|c| c.0).collect();
                    let (s, idx) = crate::preflight::bip44_path(&path, &meta).ok()?;
                    (derive_t_pubkey(&tpub, s, idx).as_ref() == Some(pk) && pays_to(pk, &hash))
                        .then_some((*pk, s, idx))
                });
                let hit = from_entry.or_else(|| search(&tpub, &hash));
                if let Some(h) = hit {
                    found.push((i, h));
                }
            }
            for (i, (pk, s, idx)) in found {
                let path = vec![44 | H, network.coin_type() | H, account | H, s, idx];
                let d = zcash_transparent::pczt::Bip32Derivation::parse(*seed_fp, path)
                    .expect("a BIP 32 path always parses");
                up.update_input_with(i, |mut iu| {
                    iu.set_bip32_derivation(pk, d);
                    Ok(())
                })?;
            }
            Ok(())
        })
        .map(Updater::finish)
        .map_err(|_| LeekError::Parse)
}

fn search(tpub: &zcash_transparent::keys::AccountPubKey, hash: &[u8; 20]) -> Option<([u8; 33], u32, u32)> {
    for scope in 0..=2 {
        for idx in 0..TRANSPARENT_SEARCH_LIMIT {
            let pk = derive_t_pubkey(tpub, scope, idx)?;
            if pays_to(&pk, hash) {
                return Some((pk, scope, idx));
            }
        }
    }
    None
}
