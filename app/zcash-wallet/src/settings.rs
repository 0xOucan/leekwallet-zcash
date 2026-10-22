//! The app's choices that outlive a run: per network, the lightwalletd
//! server when it is not the default one, and an optional NEAR Intents
//! 1Click API key.
//!
//! Kept as `settings.json` in the wallets' root, written to a temporary
//! file and renamed, like the contacts. The network on screen is the
//! webview's choice; the file still carries the `network` field of the
//! format the old desktop app wrote (and this app reads, see
//! [`crate::migrate`]).

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use zecz::Network;

/// Format version of the file.
const VERSION: u32 = 1;

/// What is saved.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Settings {
    pub version: u32,
    /// "test" or "main": the network the old desktop app was on. Kept so
    /// the file it wrote reads, and is written back, unchanged.
    pub network: String,
    /// lightwalletd endpoint per network ("test", "main") when the user set
    /// one; absent means the network's default.
    #[serde(default)]
    pub servers: BTreeMap<String, String>,
    /// An optional NEAR Intents 1Click API key (partner JWT) for swaps;
    /// `LEEKZ_1CLICK_API_KEY` overrides it. Set by hand, never shown.
    #[serde(default, rename = "oneClickApiKey", skip_serializing_if = "Option::is_none")]
    pub one_click_api_key: Option<String>,
}

impl Default for Settings {
    fn default() -> Self {
        Settings { version: VERSION, network: "test".into(), servers: BTreeMap::new(), one_click_api_key: None }
    }
}

/// "test" / "testnet" or "main" / "mainnet".
pub fn parse_network(s: &str) -> Result<Network> {
    match s.trim() {
        "test" | "testnet" => Ok(Network::TestNetwork),
        "main" | "mainnet" => Ok(Network::MainNetwork),
        other => bail!("unknown network {other:?} (test or main)"),
    }
}

/// `<root>/settings.json`.
pub fn path_in(app_data: &Path) -> PathBuf {
    app_data.join("settings.json")
}

/// Checks a lightwalletd URL the user typed: http(s)://host[:port], no
/// spaces. Whether it serves the right chain is checked when it is used.
pub fn check_server(server: &str) -> Result<String> {
    let s = server.trim();
    let rest = s.strip_prefix("https://").or_else(|| s.strip_prefix("http://"));
    match rest {
        Some(r) if !r.is_empty() && !r.contains(char::is_whitespace) && !r.starts_with('/') => Ok(s.to_string()),
        _ => bail!("enter the server as https://host:port (for example {})", zecz::wallet::default_server(Network::MainNetwork)),
    }
}

impl Settings {
    pub fn network(&self) -> Result<Network> {
        parse_network(&self.network)
    }

    /// The server the user chose for `network`, or its default.
    pub fn server_for(&self, network: Network) -> String {
        self.servers
            .get(zecz::wallet::network_name(network))
            .cloned()
            .unwrap_or_else(|| zecz::wallet::default_server(network).to_string())
    }

    /// Whether the server for `network` is the default one.
    pub fn is_default_server(&self, network: Network) -> bool {
        !self.servers.contains_key(zecz::wallet::network_name(network))
    }

    /// Sets the server for `network`; None or the default URL goes back to
    /// the default.
    pub fn set_server(&mut self, network: Network, server: Option<&str>) -> Result<()> {
        let key = zecz::wallet::network_name(network).to_string();
        match server.map(str::trim).filter(|s| !s.is_empty()) {
            None => {
                self.servers.remove(&key);
            }
            Some(s) => {
                let s = check_server(s)?;
                if s == zecz::wallet::default_server(network) {
                    self.servers.remove(&key);
                } else {
                    self.servers.insert(key, s);
                }
            }
        }
        Ok(())
    }
}

/// The saved settings; the defaults (testnet, default servers) when there
/// is no file yet.
pub fn load(path: &Path) -> Result<Settings> {
    let bytes = match fs::read(path) {
        Ok(b) => b,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Settings::default()),
        Err(e) => return Err(e).with_context(|| format!("reading {}", path.display())),
    };
    let s: Settings = serde_json::from_slice(&bytes).with_context(|| format!("{} is not a settings file", path.display()))?;
    if s.version != VERSION {
        bail!("{} has settings format {}; this app reads {VERSION}", path.display(), s.version);
    }
    s.network()?;
    for (net, server) in &s.servers {
        parse_network(net)?;
        check_server(server)?;
    }
    Ok(s)
}

/// Saves the server for `network` (None: back to the default) and returns
/// the settings.
pub fn save_server(path: &Path, network: Network, server: Option<&str>) -> Result<Settings> {
    // Preferences only: an unreadable file is replaced rather than a trap.
    let mut s = load(path).unwrap_or_default();
    s.set_server(network, server)?;
    save(path, &s)?;
    Ok(s)
}

/// Writes `s` whole: a temporary file beside it, then renamed over it.
pub fn save(path: &Path, s: &Settings) -> Result<()> {
    s.network()?;
    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir)?;
    }
    let tmp = path.with_extension("json.tmp");
    fs::write(&tmp, serde_json::to_vec_pretty(s)?).with_context(|| format!("writing {}", tmp.display()))?;
    fs::rename(&tmp, path).with_context(|| format!("replacing {}", path.display()))?;
    Ok(())
}
