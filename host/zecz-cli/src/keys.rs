//! DEV ONLY: key derivation from a BIP 39 mnemonic.
//!
//! The device derives these on-chip; the host only ever needs the UFVK it
//! exports. This module exists so `init-dev` and `sign-soft` can stand in for
//! the device during development. It reproduces the device's account shape:
//! an Orchard + P2PKH UFVK (no Sapling), ZIP 32 coin type 1 (test) / 133
//! (main), plus the ZIP 32 seed fingerprint.

use anyhow::{anyhow, Context, Result};
use orchard::keys::{FullViewingKey, SpendAuthorizingKey, SpendingKey};
use secrecy::{ExposeSecret, SecretVec};
use zcash_address::unified::{Encoding, Fvk, Revision, Ufvk, Uitem};
use zcash_keys::keys::{UnifiedFullViewingKey, UnifiedSpendingKey};
use zcash_protocol::consensus::{NetworkConstants, Parameters};
use zip32::{fingerprint::SeedFingerprint, AccountId};

pub struct DevAccount {
    pub seed: SecretVec<u8>,
    pub seed_fingerprint: SeedFingerprint,
    pub account: AccountId,
    pub orchard_sk: SpendingKey,
    pub ufvk: UnifiedFullViewingKey,
}

impl DevAccount {
    pub fn from_mnemonic<P: Parameters>(params: &P, mnemonic: &str, passphrase: &str, account: u32) -> Result<Self> {
        let m = bip39::Mnemonic::parse_normalized(mnemonic).context("invalid BIP 39 mnemonic")?;
        let seed = m.to_seed_normalized(passphrase);
        let account = AccountId::try_from(account).map_err(|_| anyhow!("account index out of range"))?;
        let seed_fingerprint = SeedFingerprint::from_seed(&seed).ok_or_else(|| anyhow!("seed too short"))?;

        let orchard_sk = SpendingKey::from_zip32_seed(&seed, params.coin_type(), account)
            .map_err(|e| anyhow!("Orchard key derivation failed: {e:?}"))?;
        let ofvk = FullViewingKey::from(&orchard_sk);
        let usk = UnifiedSpendingKey::from_seed(params, &seed, account).context("USK derivation")?;
        let full = usk.to_unified_full_viewing_key();
        let tpub = full.p2pkh().ok_or_else(|| anyhow!("no transparent key"))?;

        // Orchard + P2PKH only: the account shape the device exports. Built
        // item by item (ZIP 316 revision 0), exactly as the device encodes it.
        let items = vec![
            Uitem::Data(Fvk::Orchard(ofvk.to_bytes())),
            Uitem::Data(Fvk::P2pkh(tpub.serialize().try_into().map_err(|_| anyhow!("bad P2PKH key length"))?)),
        ];
        let encoded = Ufvk::try_from_items(Revision::R0, items)
            .map_err(|e| anyhow!("UFVK encoding: {e}"))?
            .encode(&params.network_type());
        let ufvk = UnifiedFullViewingKey::decode(params, &encoded).map_err(|e| anyhow!("UFVK decode: {e}"))?;

        Ok(DevAccount { seed: SecretVec::new(seed.to_vec()), seed_fingerprint, account, orchard_sk, ufvk })
    }

    /// The spend authorizing key for both Orchard-protocol pools (Orchard and
    /// Ironwood share the key tree).
    pub fn ask(&self) -> SpendAuthorizingKey {
        SpendAuthorizingKey::from(&self.orchard_sk)
    }

    pub fn seed_bytes(&self) -> &[u8] {
        self.seed.expose_secret()
    }
}
