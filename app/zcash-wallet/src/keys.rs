//! Which wallet a command is about, and where its files live.
//!
//! A wallet is one account on one network, owned by either
//!
//! - a board: its ZIP 32 seed fingerprint and account (`zGetAccount`). A
//!   BIP39 passphrase on the board gives another seed, so another
//!   fingerprint, and that is another wallet with its own directory and
//!   address book: a passphrase wallet never mixes with the plain one; or
//! - a bare viewing key, imported by hand (watch-only: it can see, never
//!   spend), named by the hash of that key.
//!
//! Under the root (`<app data>/zcash`):
//!
//! ```text
//! wallets/<net>-<zfp16>-<account>     a board account's wallet
//! wallets/<net>-watch-<hash16>        a viewing key's wallet
//! contacts-<net>-<zfp16|watch-…>.json one address book per seed
//! swaps-<net>-<zfp16|watch-…>.json    its NEAR Intents swaps
//! settings.json                       servers, 1Click API key
//! ```
//!
//! The first Companion builds and the old desktop app kept one wallet per
//! network (`wallets/<net>`, `contacts-<net>.json`, `swaps-<net>.json`).
//! [`settle_legacy`] moves such a wallet to the place of the account it
//! holds, with its address book and swaps, the first time this network's
//! wallets are looked at.

use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};
use serde::Serialize;
use zecz::{Network, Profile, Wallet};

/// What a wallet is named after.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Owner {
    /// A board account: the seed's ZIP 32 fingerprint and the account index.
    Board { fingerprint: [u8; 32], account: u32 },
    /// A viewing key imported by hand: the first 8 bytes of its SHA-256.
    Watch { tag: [u8; 8] },
}

/// The prefix of a watch-only wallet's name, as the webview passes it.
const WATCH: &str = "watch-";

/// Which wallet a command is about.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WalletKey {
    pub net: Network,
    pub owner: Owner,
}

impl WalletKey {
    /// The key the webview names: `wallet` is the board's seed fingerprint
    /// (64 hex digits) with its `account`, or `watch-<16 hex digits>` (the
    /// account is then ignored).
    pub fn parse(net: Network, wallet: &str, account: u32) -> Result<Self> {
        let wallet = wallet.trim();
        if let Some(tag) = wallet.strip_prefix(WATCH) {
            let tag: [u8; 8] = hex::decode(tag)
                .ok()
                .and_then(|v| v.try_into().ok())
                .context("a watch-only wallet is named watch- and 16 hex digits")?;
            return Ok(WalletKey { net, owner: Owner::Watch { tag } });
        }
        let fingerprint: [u8; 32] = hex::decode(wallet)
            .ok()
            .and_then(|v| v.try_into().ok())
            .context("the wallet's seed fingerprint is not 32 bytes of hex; read the board's account first")?;
        if account >= 0x8000_0000 {
            bail!("account {account} is out of range");
        }
        Ok(WalletKey { net, owner: Owner::Board { fingerprint, account } })
    }

    pub fn board(net: Network, fingerprint: [u8; 32], account: u32) -> Self {
        WalletKey { net, owner: Owner::Board { fingerprint, account } }
    }

    /// The wallet of the viewing key `ufvk` (trimmed, as it is imported).
    pub fn watch(net: Network, ufvk: &str) -> Self {
        let digest = zecz::zsig::sha256(ufvk.trim().as_bytes());
        let mut tag = [0u8; 8];
        tag.copy_from_slice(&digest[..8]);
        WalletKey { net, owner: Owner::Watch { tag } }
    }

    pub fn net_name(&self) -> &'static str {
        zecz::wallet::network_name(self.net)
    }

    /// What the webview calls this wallet ([`WalletKey::parse`] reads it).
    pub fn wallet(&self) -> String {
        match &self.owner {
            Owner::Board { fingerprint, .. } => hex::encode(fingerprint),
            Owner::Watch { tag } => format!("{WATCH}{}", hex::encode(tag)),
        }
    }

    pub fn account(&self) -> u32 {
        match self.owner {
            Owner::Board { account, .. } => account,
            Owner::Watch { .. } => 0,
        }
    }

    /// The part of the file names that is the seed's: the first 8 bytes of
    /// the fingerprint, enough to keep wallets apart on one device (the
    /// wallet itself records the whole origin, and an import of another
    /// seed into its directory is refused); or the viewing key's name.
    fn seed_part(&self) -> String {
        match &self.owner {
            Owner::Board { fingerprint, .. } => hex::encode(&fingerprint[..8]),
            Owner::Watch { tag } => format!("{WATCH}{}", hex::encode(tag)),
        }
    }

    /// `wallets/<net>-<zfp16>-<account>` or `wallets/<net>-watch-<hash16>`.
    pub fn wallet_dir(&self, root: &Path) -> PathBuf {
        let name = match self.owner {
            Owner::Board { account, .. } => format!("{}-{}-{account}", self.net_name(), self.seed_part()),
            Owner::Watch { .. } => format!("{}-{}", self.net_name(), self.seed_part()),
        };
        root.join("wallets").join(name)
    }

    /// `contacts-<net>-<zfp16>.json`: one book per seed, shared by its
    /// accounts, never by another seed.
    pub fn contacts_path(&self, root: &Path) -> PathBuf {
        root.join(format!("contacts-{}-{}.json", self.net_name(), self.seed_part()))
    }

    /// `swaps-<net>-<zfp16>.json`: the NEAR Intents swaps paid from or into
    /// this seed's wallets.
    pub fn swaps_path(&self, root: &Path) -> PathBuf {
        root.join(format!("swaps-{}-{}.json", self.net_name(), self.seed_part()))
    }
}

/// The key of the account held in the wallet at `p`, if it holds one.
pub fn key_of_wallet(p: &Profile) -> Result<Option<WalletKey>> {
    if !p.exists() {
        return Ok(None);
    }
    let w = Wallet::open(p.clone())?;
    if !w.has_account()? {
        return Ok(None);
    }
    Ok(Some(match w.key_origin()? {
        Some(o) => WalletKey::board(p.network, o.seed_fingerprint, o.account_index),
        None => WalletKey::watch(p.network, &w.ufvk()?),
    }))
}

/// `wallets/<net>`: the one wallet per network of the old layout.
pub fn legacy_wallet_dir(root: &Path, net: Network) -> PathBuf {
    root.join("wallets").join(zecz::wallet::network_name(net))
}

/// `swaps-<net>.json` of the old layout.
pub fn legacy_swaps_path(root: &Path, net: Network) -> PathBuf {
    root.join(format!("swaps-{}.json", zecz::wallet::network_name(net)))
}

/// Moves `from` to `to` unless `to` exists. Returns whether it moved.
fn move_if_free(from: &Path, to: &Path) -> Result<bool> {
    if !from.exists() || to.exists() {
        return Ok(false);
    }
    if let Some(parent) = to.parent() {
        std::fs::create_dir_all(parent)?;
    }
    std::fs::rename(from, to).with_context(|| format!("moving {} to {}", from.display(), to.display()))?;
    Ok(true)
}

/// Moves the old layout's wallet of `net` (and its address book and swaps)
/// to the place of the account it holds, when that place is free. Returns
/// the key it settled under, if it moved.
///
/// A directory that holds no wallet with an account stays where it is, and
/// so does one whose account already has a wallet in the new layout (the
/// newer wallet wins; nothing is overwritten).
pub fn settle_legacy(root: &Path, net: Network) -> Result<Option<WalletKey>> {
    let legacy = legacy_wallet_dir(root, net);
    if !legacy.is_dir() {
        return Ok(None);
    }
    let Some(key) = key_of_wallet(&Profile::new(net, legacy.clone(), None))? else {
        return Ok(None);
    };
    if !move_if_free(&legacy, &key.wallet_dir(root))? {
        return Ok(None);
    }
    move_if_free(&crate::contacts::path_in(root, net), &key.contacts_path(root))?;
    move_if_free(&legacy_swaps_path(root, net), &key.swaps_path(root))?;
    Ok(Some(key))
}

/// A wallet on this device, as the wallet list shows it.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct WalletInfo {
    /// "test" or "main".
    pub network: String,
    /// What the commands take as `wallet` (with `account`).
    pub wallet: String,
    pub account: u32,
    /// A viewing key imported by hand: it can never sign.
    pub watch_only: bool,
    /// The board's short seed fingerprint ("ZFP"), for a board account.
    pub zfp: Option<String>,
    pub birthday: Option<u32>,
    /// The last block scanned, if it was ever synced.
    pub fully_scanned: Option<u32>,
}

/// The wallets of `net` on this device, board accounts first, after
/// settling the old layout's. A directory that cannot be read is skipped
/// (and said on stderr): one broken wallet must not hide the others.
pub fn list_wallets(root: &Path, net: Network) -> Result<Vec<WalletInfo>> {
    if let Err(e) = settle_legacy(root, net) {
        eprintln!("zcash: the old {} wallet was not moved: {e:#}", zecz::wallet::network_name(net));
    }
    let dir = root.join("wallets");
    let entries = match std::fs::read_dir(&dir) {
        Ok(e) => e,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(e) => return Err(e).with_context(|| format!("reading {}", dir.display())),
    };
    let prefix = format!("{}-", zecz::wallet::network_name(net));
    let mut out = Vec::new();
    for entry in entries {
        let path = entry?.path();
        let named = path.file_name().and_then(|n| n.to_str()).is_some_and(|n| n.starts_with(&prefix));
        if !named || !path.is_dir() {
            continue;
        }
        match info(&Profile::new(net, path.clone(), None)) {
            Ok(Some(i)) => out.push(i),
            Ok(None) => {}
            Err(e) => eprintln!("zcash: skipping {}: {e:#}", path.display()),
        }
    }
    out.sort_by(|a, b| (a.watch_only, &a.zfp, a.account, &a.wallet).cmp(&(b.watch_only, &b.zfp, b.account, &b.wallet)));
    Ok(out)
}

fn info(p: &Profile) -> Result<Option<WalletInfo>> {
    let Some(key) = key_of_wallet(p)? else {
        return Ok(None);
    };
    // A directory named for one account that holds another is not listed
    // under the wrong name.
    if p.data_dir.file_name() != key.wallet_dir(Path::new("")).file_name() {
        bail!("it holds another account than its name says");
    }
    let w = Wallet::open(p.clone())?;
    let zfp = match &key.owner {
        Owner::Board { fingerprint, .. } => Some(zecz::wallet::short_fingerprint(fingerprint)),
        Owner::Watch { .. } => None,
    };
    Ok(Some(WalletInfo {
        network: key.net_name().to_string(),
        wallet: key.wallet(),
        account: key.account(),
        watch_only: matches!(key.owner, Owner::Watch { .. }),
        zfp,
        birthday: Some(w.birthday()?),
        fully_scanned: w.sync_status()?.map(|s| s.fully_scanned),
    }))
}

#[cfg(test)]
mod tests {
    use super::*;

    const TEST: Network = Network::TestNetwork;

    #[test]
    fn a_wallet_key_needs_the_whole_fingerprint_or_a_watch_name() {
        assert!(WalletKey::parse(TEST, "", 0).is_err());
        assert!(WalletKey::parse(TEST, "abcd", 0).is_err());
        assert!(WalletKey::parse(TEST, &"zz".repeat(32), 0).is_err());
        assert!(WalletKey::parse(TEST, &"ab".repeat(32), 0x8000_0000).is_err());
        assert!(WalletKey::parse(TEST, &format!(" {} ", "AB".repeat(32)), 7).is_ok());
        assert!(WalletKey::parse(TEST, "watch-0011223344556677", 0).is_ok());
        assert!(WalletKey::parse(TEST, "watch-00112233", 0).is_err());
        assert!(WalletKey::parse(TEST, "watch-", 0).is_err());
    }

    #[test]
    fn names_go_both_ways() {
        let b = WalletKey::board(TEST, [0xab; 32], 3);
        assert_eq!(WalletKey::parse(TEST, &b.wallet(), b.account()).unwrap(), b);
        let w = WalletKey::watch(TEST, " uviewtest1example ");
        assert_eq!(w, WalletKey::watch(TEST, "uviewtest1example"));
        assert!(w.wallet().starts_with("watch-") && w.wallet().len() == 6 + 16);
        assert_eq!(WalletKey::parse(TEST, &w.wallet(), 9).unwrap(), w);
        assert_eq!(w.account(), 0);
    }

    #[test]
    fn every_wallet_has_its_own_places() {
        let root = Path::new("/z");
        let fp = [0x12; 32];
        let k0 = WalletKey::board(TEST, fp, 0);
        let mut other = fp;
        other[0] = 0x13; // another seed: what a BIP39 passphrase gives
        let k1 = WalletKey::board(TEST, other, 0);
        assert_eq!(k0.wallet_dir(root), root.join("wallets/test-1212121212121212-0"));
        assert_ne!(k0.wallet_dir(root), k1.wallet_dir(root));
        assert_ne!(k0.contacts_path(root), k1.contacts_path(root));
        assert_ne!(k0.swaps_path(root), k1.swaps_path(root));
        // Another account of one seed: another wallet, the same book.
        let a1 = WalletKey::board(TEST, fp, 1);
        assert_ne!(k0.wallet_dir(root), a1.wallet_dir(root));
        assert_eq!(k0.contacts_path(root), a1.contacts_path(root));
        // The other network: never the same place.
        let m = WalletKey::board(Network::MainNetwork, fp, 0);
        assert_ne!(k0.wallet_dir(root), m.wallet_dir(root));
        assert_ne!(k0.contacts_path(root), m.contacts_path(root));
        assert_ne!(k0.swaps_path(root), m.swaps_path(root));
        // A viewing key: its own places too, never the old layout's.
        let w = WalletKey::watch(TEST, "uviewtest1example");
        assert!(w.wallet_dir(root).to_str().unwrap().contains("wallets/test-watch-"));
        for k in [&k0, &w] {
            assert_ne!(k.wallet_dir(root), legacy_wallet_dir(root, TEST));
            assert_ne!(k.contacts_path(root), crate::contacts::path_in(root, TEST));
            assert_ne!(k.swaps_path(root), legacy_swaps_path(root, TEST));
        }
    }

    #[test]
    fn an_old_directory_without_a_wallet_stays_put() {
        let root = std::env::temp_dir().join(format!("leek-keys-legacy-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let legacy = legacy_wallet_dir(&root, TEST);
        std::fs::create_dir_all(&legacy).unwrap();
        std::fs::write(legacy.join("marker"), b"x").unwrap();
        assert_eq!(settle_legacy(&root, TEST).unwrap(), None);
        assert!(legacy.join("marker").exists(), "the old directory was moved");
        assert!(list_wallets(&root, TEST).unwrap().is_empty());
        let _ = std::fs::remove_dir_all(&root);
    }
}
