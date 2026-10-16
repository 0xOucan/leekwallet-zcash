//! `.zkv` seed cards from a mnemonic, and what the device will load from
//! one: the operations behind `zecz zkv-new` and `zecz zkv-open`. The file
//! format itself is in [`crate::zkv`].

use std::path::Path;

use anyhow::{bail, Context, Result};
use zcash_protocol::consensus::Network;

use crate::keys::DevAccount;
use crate::wallet::short_fingerprint;
use crate::zkv;

/// What to seal into a new card.
#[derive(Clone, Debug)]
pub struct NewCard<'a> {
    pub mnemonic: &'a str,
    /// The file passphrase.
    pub passphrase: &'a str,
    pub birthday: u32,
    pub account: u32,
    pub label: &'a str,
    pub iterations: u32,
}

/// Checks a BIP 39 mnemonic before asking for anything else.
pub fn check_mnemonic(mnemonic: &str) -> Result<()> {
    bip39::Mnemonic::parse_normalized(mnemonic).context("invalid BIP 39 mnemonic")?;
    Ok(())
}

/// Seals `card` into a new file at `out` and reads it back. Refuses to
/// overwrite. Returns the file's size in bytes.
pub fn write_new(card: &NewCard, out: &Path) -> Result<usize> {
    let m = bip39::Mnemonic::parse_normalized(card.mnemonic).context("invalid BIP 39 mnemonic")?;
    let c = zkv::Contents { entropy: m.to_entropy(), birthday: card.birthday, account: card.account, flags: 0, label: card.label.to_string() };
    let file = zkv::seal(&c, card.passphrase, card.iterations)?;
    if out.exists() {
        bail!("{} exists; the device never overwrites a seed file and neither does this", out.display());
    }
    std::fs::write(out, &file)?;
    // Read it back before saying it worked: a card that cannot be opened
    // is not a backup.
    if zkv::open(&std::fs::read(out)?, card.passphrase)? != c {
        bail!("{} did not read back", out.display());
    }
    Ok(file.len())
}

/// What the device will load from a card. Never includes the phrase.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CardInfo {
    pub words: usize,
    pub iterations: u32,
    pub label: String,
    pub account: u32,
    pub birthday: u32,
    pub flags: u8,
    /// Short seed fingerprint, as the device shows it.
    pub short_fingerprint: String,
    pub ufvk: String,
}

/// Opens a card with its file passphrase and derives the account as the
/// device would, with `bip39_passphrase` applied on top.
pub fn describe(file: &[u8], passphrase: &str, bip39_passphrase: &str, network: Network) -> Result<CardInfo> {
    let iterations = zkv::peek(file)?;
    let c = zkv::open(file, passphrase)?;
    let m = bip39::Mnemonic::from_entropy(&c.entropy).context("entropy")?;
    let acct = DevAccount::from_mnemonic(&network, &m.to_string(), bip39_passphrase, c.account)?;
    Ok(CardInfo {
        words: m.word_count(),
        iterations,
        label: c.label.clone(),
        account: c.account,
        birthday: c.birthday,
        flags: c.flags,
        short_fingerprint: short_fingerprint(&acct.seed_fingerprint.to_bytes()),
        ufvk: acct.ufvk.encode(&network)?,
    })
}
