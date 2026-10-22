//! The address book: names for addresses the user pays, kept on this
//! device only, as JSON beside the wallets (one file per network and seed,
//! [`crate::keys::WalletKey::contacts_path`]: an address belongs to one
//! network, and a passphrase wallet keeps its own book).
//!
//! The file is small and rewritten whole: written to a temporary file
//! beside it, then renamed over it, so a crash leaves the old book or the
//! new one, never half of one.

use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use zecz::Network;

/// The longest name kept, in characters.
pub const MAX_NAME: usize = 64;
/// Format version of the file.
const VERSION: u32 = 1;

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Contact {
    pub name: String,
    pub address: String,
}

#[derive(Debug, Serialize, Deserialize)]
struct Book {
    version: u32,
    contacts: Vec<Contact>,
}

/// `<root>/contacts-<test|main>.json`: the one book per network of the old
/// desktop app and the first Companion builds, which [`crate::keys`]
/// settles into the book of the wallet it belonged to.
pub fn path_in(app_data: &Path, network: Network) -> PathBuf {
    app_data.join(format!("contacts-{}.json", zecz::wallet::network_name(network)))
}

/// The contacts, sorted by name; none when the file does not exist yet.
pub fn list(path: &Path) -> Result<Vec<Contact>> {
    let bytes = match fs::read(path) {
        Ok(b) => b,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(e) => return Err(e).with_context(|| format!("reading {}", path.display())),
    };
    let book: Book = serde_json::from_slice(&bytes).with_context(|| format!("{} is not a contacts file", path.display()))?;
    if book.version != VERSION {
        bail!("{} has contacts format {}; this app reads {VERSION}", path.display(), book.version);
    }
    let mut c = book.contacts;
    sort(&mut c);
    Ok(c)
}

fn sort(c: &mut [Contact]) {
    c.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()).then_with(|| a.name.cmp(&b.name)));
}

fn write(path: &Path, contacts: Vec<Contact>) -> Result<()> {
    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir)?;
    }
    let json = serde_json::to_vec_pretty(&Book { version: VERSION, contacts })?;
    let tmp = path.with_extension("json.tmp");
    fs::write(&tmp, json).with_context(|| format!("writing {}", tmp.display()))?;
    fs::rename(&tmp, path).with_context(|| format!("replacing {}", path.display()))?;
    Ok(())
}

/// Adds a contact, or changes the one called `replacing` (its name or its
/// address). Names are unique, ignoring case; the address must be one this
/// wallet can pay on `network`. Returns the book after the change.
pub fn save(path: &Path, network: Network, name: &str, address: &str, replacing: Option<&str>) -> Result<Vec<Contact>> {
    let name = name.trim();
    let address = address.trim();
    if name.is_empty() {
        bail!("enter a name");
    }
    if name.chars().count() > MAX_NAME {
        bail!("the name is longer than {MAX_NAME} characters");
    }
    if name.chars().any(char::is_control) {
        bail!("the name has control characters");
    }
    zecz::wallet::check_address(network, address)?;
    let mut contacts = list(path)?;
    if let Some(old) = replacing {
        let before = contacts.len();
        contacts.retain(|c| c.name != old);
        if contacts.len() == before {
            bail!("there is no contact called {old:?}");
        }
    }
    if let Some(c) = contacts.iter().find(|c| c.name.to_lowercase() == name.to_lowercase()) {
        bail!("there is already a contact called {:?}", c.name);
    }
    contacts.push(Contact { name: name.to_string(), address: address.to_string() });
    sort(&mut contacts);
    write(path, contacts.clone())?;
    Ok(contacts)
}

/// Removes the contact called `name`. Returns the book after the change.
pub fn remove(path: &Path, name: &str) -> Result<Vec<Contact>> {
    let mut contacts = list(path)?;
    let before = contacts.len();
    contacts.retain(|c| c.name != name);
    if contacts.len() == before {
        bail!("there is no contact called {name:?}");
    }
    write(path, contacts.clone())?;
    Ok(contacts)
}
