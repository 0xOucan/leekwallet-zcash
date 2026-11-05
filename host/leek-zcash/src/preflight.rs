//! Host-side checks before anything reaches the board.

use orchard::keys::FullViewingKey;
use orchard::pczt::VerifyError;
use pczt::roles::verifier::{OrchardError, TransparentError, Verifier};
use pczt::Pczt;
use zcash_keys::keys::UnifiedFullViewingKey;
use zcash_transparent::address::TransparentAddress;
use zcash_transparent::keys::{AccountPubKey, NonHardenedChildIndex, TransparentKeyScope};

use crate::account::{AccountMeta, Features, Network};
use crate::error::{LeekError, Unsupported};
use crate::redact::{signer_view_bytes, SignerView};

pub(crate) const H: u32 = 0x8000_0000;

/// The keys of an account, from its UFVK.
pub(crate) struct Keys {
    pub orchard: Option<FullViewingKey>,
    pub transparent: Option<AccountPubKey>,
}

pub(crate) fn decode_ufvk(ufvk: &str, network: Network) -> Result<Keys, LeekError> {
    let k = UnifiedFullViewingKey::decode(&network.params(), ufvk).map_err(|_| {
        // Another network's UFVK fails to decode as this one's.
        let other = match network {
            Network::Main => Network::Test,
            Network::Test => Network::Main,
        };
        if UnifiedFullViewingKey::decode(&other.params(), ufvk).is_ok() {
            LeekError::WrongNetwork
        } else {
            LeekError::Parse
        }
    })?;
    Ok(Keys { orchard: k.orchard().cloned(), transparent: k.p2pkh().cloned() })
}

/// Whether a verification failed only for lack of data (the board then
/// checks with its own keys), rather than because the key is not ours.
pub(crate) fn only_missing(e: &VerifyError) -> bool {
    matches!(
        e,
        VerifyError::MissingRandomSeed
            | VerifyError::MissingRecipient
            | VerifyError::MissingRho
            | VerifyError::MissingSpendAuthRandomizer
            | VerifyError::MissingValue
            | VerifyError::MissingFullViewingKey
    )
}

/// `Some(true)` if the spend provably derives from `fvk`, `Some(false)` if
/// provably not, `None` if the PCZT lacks the data to tell.
pub(crate) fn spend_is_ours(spend: &orchard::pczt::Spend, fvk: &FullViewingKey) -> Option<bool> {
    for r in [spend.verify_nullifier(Some(fvk)), spend.verify_rk(Some(fvk))] {
        match r {
            Ok(()) => {}
            Err(e) if only_missing(&e) => return None,
            Err(_) => return Some(false),
        }
    }
    Some(true)
}

/// Checks a PCZT against the account and the board's features on the host,
/// before anything is sent, so a wallet gets a typed answer at once rather
/// than a refusal after an upload:
///
/// - the coin type is the account's network ([`LeekError::WrongNetwork`]);
/// - the transaction version and consensus branch are ones the board signs
///   ([`Unsupported::Version`], [`Unsupported::Branch`]);
/// - no Sapling spend or output ([`Unsupported::Sapling`]);
/// - no two Orchard or Ironwood actions, both pools together, share an `rk`
///   ([`LeekError::DuplicateRk`]);
/// - at most [`Features::max_transparent_inputs`] transparent inputs
///   ([`LeekError::TooMany`]), each plain P2PKH with SIGHASH_ALL
///   ([`Unsupported::Pool`]) and exactly one BIP 32 derivation that names
///   this seed fingerprint, coin type and account, at scope 0, 1 or 2, whose
///   key the script pays to and (with a UFVK) the account derives
///   ([`LeekError::MissingDerivation`], [`LeekError::WrongAccount`],
///   [`LeekError::SpendNotOurs`]);
/// - every unsigned Orchard and Ironwood spend: a ZIP 32 derivation, if
///   present, names this seed and account; with a UFVK, the note's nullifier
///   and `rk` derive from its Orchard key ([`LeekError::SpendNotOurs`]); no
///   dummy key ([`LeekError::Parse`]); a spend another signer already signed
///   carries no value ([`Unsupported::Multistep`]);
/// - the compact signer view fits the board ([`LeekError::TooLarge`]).
///
/// The board makes every one of these checks again, and more (outputs,
/// addresses, memos, value balance); preflight only makes failures early
/// and precise.
pub fn preflight(pczt: &Pczt, account: &AccountMeta, features: &Features) -> Result<(), LeekError> {
    let g = pczt.global();
    match Network::from_coin_type(*g.coin_type()) {
        Some(n) if n == account.network => {}
        Some(_) => return Err(LeekError::WrongNetwork),
        None => return Err(LeekError::Unsupported(Unsupported::Network)),
    }
    if !features.tx_versions.contains(g.tx_version()) {
        return Err(LeekError::Unsupported(Unsupported::Version));
    }
    if !features.branch_ids.contains(g.consensus_branch_id()) {
        return Err(LeekError::Unsupported(Unsupported::Branch));
    }
    if !pczt.sapling().spends().is_empty() || !pczt.sapling().outputs().is_empty() {
        return Err(LeekError::Unsupported(Unsupported::Sapling));
    }
    // Structural, like the board's ZS_E_DUPLICATE_RK and Keystone's rule:
    // raw rk bytes of every action, both pools together, signed or not.
    let mut rks = std::collections::HashSet::new();
    if !pczt.orchard().actions().iter().chain(pczt.ironwood().actions()).all(|a| rks.insert(*a.spend().rk())) {
        return Err(LeekError::DuplicateRk);
    }
    let n_t = pczt.transparent().inputs().len();
    if n_t > features.max_transparent_inputs {
        return Err(LeekError::TooMany);
    }
    let pool_ok = |name: &str| features.spend_pools.iter().any(|p| p == name);
    let unsigned = |b: &pczt::orchard::Bundle| b.actions().iter().any(|a| a.spend().spend_auth_sig().is_none());
    if (n_t > 0 && !pool_ok("transparent"))
        || (unsigned(pczt.orchard()) && !pool_ok("orchard"))
        || (unsigned(pczt.ironwood()) && !pool_ok("ironwood"))
    {
        return Err(LeekError::Unsupported(Unsupported::Pool));
    }

    let keys = match &account.ufvk {
        Some(u) => decode_ufvk(u, account.network)?,
        None => Keys { orchard: None, transparent: None },
    };
    let mut v = Verifier::new(pczt.clone());
    if n_t > 0 {
        v = v
            .with_transparent(|b| {
                for input in b.inputs() {
                    check_transparent(input, account, keys.transparent.as_ref()).map_err(TransparentError::Custom)?;
                }
                Ok(())
            })
            .map_err(|e| match e {
                TransparentError::Custom(l) => l,
                _ => LeekError::Parse,
            })?;
    }
    let shielded = |b: &orchard::pczt::Bundle| -> Result<(), OrchardError<LeekError>> {
        for a in b.actions() {
            check_spend(a.spend(), account, keys.orchard.as_ref()).map_err(OrchardError::Custom)?;
        }
        Ok(())
    };
    let orchard_err = |e: OrchardError<LeekError>| match e {
        OrchardError::Custom(l) => l,
        OrchardError::UnsupportedConsensusBranchId => LeekError::Unsupported(Unsupported::Branch),
        _ => LeekError::Parse,
    };
    if !pczt.orchard().actions().is_empty() {
        v = v.with_orchard(shielded).map_err(orchard_err)?;
    }
    if !pczt.ironwood().actions().is_empty() {
        v = v.with_ironwood(shielded).map_err(orchard_err)?;
    }
    drop(v);

    // The smallest view this crate can send must fit.
    if signer_view_bytes(pczt, SignerView::Compact)?.len() > features.max_pczt_bytes {
        return Err(LeekError::TooLarge);
    }
    Ok(())
}

fn check_spend(spend: &orchard::pczt::Spend, account: &AccountMeta, fvk: Option<&FullViewingKey>) -> Result<(), LeekError> {
    if spend.dummy_sk().is_some() {
        return Err(LeekError::Parse);
    }
    if spend.spend_auth_sig().is_some() {
        // Pre-signed: acceptable only as zero-value padding.
        return match spend.value() {
            Some(v) if v.inner() > 0 => Err(LeekError::Unsupported(Unsupported::Multistep)),
            _ => Ok(()),
        };
    }
    if let Some(d) = spend.zip32_derivation() {
        if *d.seed_fingerprint() != [0; 32] {
            if *d.seed_fingerprint() != account.seed_fingerprint {
                return Err(LeekError::WrongAccount);
            }
            let path: Vec<u32> = d.derivation_path().iter().map(|c| c.index()).collect();
            match path[..] {
                [p, c, a] if p == 32 | H && c == account.network.coin_type() | H => {
                    if a != account.index | H {
                        return Err(LeekError::WrongAccount);
                    }
                }
                [p, _, _] if p == 32 | H => return Err(LeekError::WrongNetwork),
                _ => return Err(LeekError::SpendNotOurs),
            }
        }
    }
    if let Some(fvk) = fvk {
        if spend_is_ours(spend, fvk) == Some(false) {
            return Err(LeekError::SpendNotOurs);
        }
    }
    Ok(())
}

/// The `(scope, index)` of a BIP 44 path of this account, or the error the
/// path deserves.
pub(crate) fn bip44_path(path: &[u32], account: &AccountMeta) -> Result<(u32, u32), LeekError> {
    match *path {
        [p, c, a, s, i] if p == 44 | H && s < H && i < H => {
            if c != account.network.coin_type() | H {
                Err(LeekError::WrongNetwork)
            } else if a != account.index | H {
                Err(LeekError::WrongAccount)
            } else if s > 2 {
                Err(LeekError::SpendNotOurs)
            } else {
                Ok((s, i))
            }
        }
        _ => Err(LeekError::SpendNotOurs),
    }
}

pub(crate) fn derive_t_pubkey(tpub: &AccountPubKey, scope: u32, index: u32) -> Option<[u8; 33]> {
    let scope = match scope {
        0 => TransparentKeyScope::EXTERNAL,
        1 => TransparentKeyScope::INTERNAL,
        2 => TransparentKeyScope::EPHEMERAL,
        _ => return None,
    };
    let index = NonHardenedChildIndex::from_index(index)?;
    tpub.derive_address_pubkey(scope, index).ok().map(|k| k.serialize())
}

pub(crate) fn p2pkh_hash(input: &zcash_transparent::pczt::Input) -> Option<[u8; 20]> {
    match TransparentAddress::from_script_from_chain(input.script_pubkey()) {
        Some(TransparentAddress::PublicKeyHash(h)) => Some(h),
        _ => None,
    }
}

pub(crate) fn pays_to(pubkey: &[u8; 33], hash: &[u8; 20]) -> bool {
    secp256k1::PublicKey::from_slice(pubkey)
        .map(|pk| TransparentAddress::from_pubkey(&pk) == TransparentAddress::PublicKeyHash(*hash))
        .unwrap_or(false)
}

fn check_transparent(
    input: &zcash_transparent::pczt::Input,
    account: &AccountMeta,
    tpub: Option<&AccountPubKey>,
) -> Result<(), LeekError> {
    let hash = p2pkh_hash(input).ok_or(LeekError::Unsupported(Unsupported::Pool))?;
    if input.sighash_type().encode() != 0x01
        || input.redeem_script().is_some()
        || input.script_sig().is_some()
        || input.required_time_lock_time().is_some()
        || input.required_height_lock_time().is_some()
    {
        return Err(LeekError::Unsupported(Unsupported::Pool));
    }
    if !input.partial_signatures().is_empty() {
        return Err(LeekError::Unsupported(Unsupported::Multistep));
    }
    let mut entries = input.bip32_derivation().iter();
    let (pubkey, d) = match (entries.next(), entries.next()) {
        (Some(e), None) => e,
        (None, _) => return Err(LeekError::MissingDerivation),
        (Some(_), Some(_)) => return Err(LeekError::Unsupported(Unsupported::Pool)),
    };
    if *d.seed_fingerprint() == [0; 32] {
        return Err(LeekError::MissingDerivation);
    }
    if *d.seed_fingerprint() != account.seed_fingerprint {
        return Err(LeekError::WrongAccount);
    }
    let path: Vec<u32> = d.derivation_path().iter().map(|c| c.0).collect();
    let (scope, index) = bip44_path(&path, account)?;
    if !pays_to(pubkey, &hash) {
        return Err(LeekError::SpendNotOurs);
    }
    if let Some(tpub) = tpub {
        if derive_t_pubkey(tpub, scope, index).as_ref() != Some(pubkey) {
            return Err(LeekError::SpendNotOurs);
        }
    }
    Ok(())
}
