//! The first start after the old desktop app.
//!
//! Before the two apps became one, the desktop-only "LeekWallet Zcash"
//! (identifier `com.leekwallet.zcash`) kept one wallet per network in its
//! own data directory:
//!
//! ```text
//! wallets/<test|main>/        the wallet (watch-only, or a board account)
//! contacts-<test|main>.json   the address book of that network
//! swaps-<test|main>.json      its NEAR Intents swaps
//! settings.json               servers, 1Click API key
//! ```
//!
//! [`from_desktop`] copies them into this app's layout ([`crate::keys`]) on
//! the first start that finds them: each wallet is copied to a staging
//! directory, opened there to learn which account it holds, and moved to
//! that account's place; its address book and swaps follow it. Nothing is
//! overwritten: a wallet, book or swaps file this app already has for that
//! account wins, and the copy is dropped. The old directory is only ever
//! read, never changed or removed, so the old app keeps working and the
//! copy can be done again by removing [`MARKER`].
//!
//! Once done (or found to have nothing to do) it is recorded in
//! [`MARKER`] under the root, with what was copied, and never runs again.

use std::path::{Path, PathBuf};

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use zecz::{Network, Profile};

use crate::keys::{self, WalletKey};
use crate::{contacts, settings};

/// The old desktop app's identifier, which named its data directory.
pub const OLD_IDENTIFIER: &str = "com.leekwallet.zcash";

/// Written under the root once the copy was done.
pub const MARKER: &str = "migrated-from-desktop.json";

/// The old app's data directory, given this app's: Tauri puts both at
/// `<platform data dir>/<identifier>` (`~/.local/share` on Linux,
/// `~/Library/Application Support` on macOS, `%APPDATA%` on Windows).
pub fn old_dir_beside(app_data: &Path) -> Option<PathBuf> {
    app_data.parent().map(|p| p.join(OLD_IDENTIFIER))
}

/// What a copy did, kept in [`MARKER`].
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Report {
    /// The old data directory.
    pub from: String,
    /// What was copied, and where to, in words.
    pub copied: Vec<String>,
    /// What was found and left, and why.
    pub skipped: Vec<String>,
}

/// Copies the old desktop app's wallets, address books, swaps and settings
/// from `old` into `root` (`<this app's data>/zcash`), once. Returns the
/// report, or None when there was nothing to do: the copy was already
/// made, or there is no old directory.
pub fn from_desktop(old: &Path, root: &Path) -> Result<Option<Report>> {
    let marker = root.join(MARKER);
    if marker.exists() || !old.is_dir() {
        return Ok(None);
    }
    let mut report = Report { from: old.display().to_string(), ..Report::default() };
    for net in [Network::TestNetwork, Network::MainNetwork] {
        network(old, root, net, &mut report).with_context(|| format!("copying the old {} wallet", zecz::wallet::network_name(net)))?;
    }
    settings_from(old, root, &mut report)?;
    std::fs::create_dir_all(root)?;
    std::fs::write(&marker, serde_json::to_vec_pretty(&report)?).with_context(|| format!("writing {}", marker.display()))?;
    Ok(Some(report))
}

/// One network: the wallet, then its book and swaps where it went.
fn network(old: &Path, root: &Path, net: Network, report: &mut Report) -> Result<()> {
    let name = zecz::wallet::network_name(net);
    let old_wallet = keys::legacy_wallet_dir(old, net);
    let old_book = contacts::path_in(old, net);
    let old_swaps = keys::legacy_swaps_path(old, net);

    let key = if Profile::new(net, old_wallet.clone(), None).exists() { wallet(&old_wallet, root, net, report)? } else { None };
    let (book, swaps) = match &key {
        Some(k) => (k.contacts_path(root), k.swaps_path(root)),
        // No account to put them with: the old layout's places, which
        // follow the wallet that turns up there (keys::settle_legacy).
        None => (contacts::path_in(root, net), keys::legacy_swaps_path(root, net)),
    };
    copy_file(&old_book, &book, &format!("the {name} address book"), report)?;
    copy_file(&old_swaps, &swaps, &format!("the {name} swaps"), report)?;
    Ok(())
}

/// Copies the old wallet of `net` and moves the copy to the place of the
/// account it holds. Returns that account's key.
fn wallet(old_wallet: &Path, root: &Path, net: Network, report: &mut Report) -> Result<Option<WalletKey>> {
    let name = zecz::wallet::network_name(net);
    let staging = root.join("migrating").join(name);
    let _ = std::fs::remove_dir_all(&staging);
    copy_dir(old_wallet, &staging).with_context(|| format!("copying {}", old_wallet.display()))?;
    let key = match keys::key_of_wallet(&Profile::new(net, staging.clone(), None)) {
        Ok(k) => k,
        Err(e) => {
            report.skipped.push(format!("the {name} wallet: it does not open ({e:#})"));
            None
        }
    };
    let Some(key) = key else {
        let _ = std::fs::remove_dir_all(root.join("migrating"));
        report.skipped.push(format!("the {name} wallet: it holds no account"));
        return Ok(None);
    };
    let dir = key.wallet_dir(root);
    if dir.exists() {
        report.skipped.push(format!("the {name} wallet: this app already has it ({})", dir.display()));
    } else {
        std::fs::create_dir_all(dir.parent().context("a wallet directory has a parent")?)?;
        std::fs::rename(&staging, &dir).with_context(|| format!("moving the copy to {}", dir.display()))?;
        report.copied.push(format!("the {name} wallet to {}", dir.display()));
    }
    let _ = std::fs::remove_dir_all(root.join("migrating"));
    Ok(Some(key))
}

/// Copies one file unless the destination exists.
fn copy_file(from: &Path, to: &Path, what: &str, report: &mut Report) -> Result<()> {
    if !from.is_file() {
        return Ok(());
    }
    if to.exists() {
        report.skipped.push(format!("{what}: this app already has one ({})", to.display()));
        return Ok(());
    }
    if let Some(dir) = to.parent() {
        std::fs::create_dir_all(dir)?;
    }
    std::fs::copy(from, to).with_context(|| format!("copying {} to {}", from.display(), to.display()))?;
    report.copied.push(format!("{what} to {}", to.display()));
    Ok(())
}

/// A directory tree, files and directories only (a wallet has nothing
/// else; a link is not followed out of it).
fn copy_dir(from: &Path, to: &Path) -> Result<()> {
    std::fs::create_dir_all(to)?;
    for entry in std::fs::read_dir(from)? {
        let entry = entry?;
        let kind = entry.file_type()?;
        let target = to.join(entry.file_name());
        if kind.is_dir() {
            copy_dir(&entry.path(), &target)?;
        } else if kind.is_file() {
            std::fs::copy(entry.path(), &target)?;
        }
    }
    Ok(())
}

/// The old servers and 1Click API key, where this app has none of its own.
fn settings_from(old: &Path, root: &Path, report: &mut Report) -> Result<()> {
    let from = settings::path_in(old);
    if !from.is_file() {
        return Ok(());
    }
    let theirs = match settings::load(&from) {
        Ok(s) => s,
        Err(e) => {
            report.skipped.push(format!("the settings: {e:#}"));
            return Ok(());
        }
    };
    let to = settings::path_in(root);
    // An unreadable file of this app's own is left for the user to fix.
    let Ok(mut ours) = settings::load(&to) else {
        report.skipped.push(format!("the settings: {} does not read", to.display()));
        return Ok(());
    };
    let before = ours.clone();
    for (net, server) in theirs.servers {
        ours.servers.entry(net).or_insert(server);
    }
    if ours.one_click_api_key.is_none() {
        ours.one_click_api_key = theirs.one_click_api_key;
    }
    if ours != before {
        settings::save(&to, &ours)?;
        report.copied.push("the lightwalletd servers and the 1Click API key that were set".into());
    }
    Ok(())
}
