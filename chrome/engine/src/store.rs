//! Storage: SQLite on OPFS, through sqlite-wasm-vfs's sync access handle
//! pool (the `opfs-sahpool` VFS of sqlite-wasm, ported to Rust).
//!
//! Each network has its own pair of files in the pool:
//! - `/<net>/wallet.sqlite`: zcash_client_sqlite's wallet database, the
//!   same schema zecz keeps in `wallet.sqlite`;
//! - `/<net>/leek.sqlite`: what zecz keeps beside it as files: the full
//!   PCZT of each pending send (zecz's `pending/`), its signed and proven
//!   stages, and each raw transaction built (zecz's `sent/`).
//!
//! OPFS is per extension origin and survives reloads, updates and browser
//! restarts; it goes when the extension is uninstalled. Sync access handles
//! exist only in dedicated workers, which is where the engine runs.

use std::cell::RefCell;

use anyhow::{anyhow, Context, Result};
use rusqlite::Connection;
use sqlite_wasm_vfs::sahpool::{install, OpfsSAHPoolCfgBuilder, OpfsSAHPoolUtil};

/// The OPFS directory the pool keeps its files in.
pub const OPFS_DIR: &str = "leekwallet-zcash";

thread_local! {
    static POOL: RefCell<Option<OpfsSAHPoolUtil>> = const { RefCell::new(None) };
}

/// Installs the OPFS pool as SQLite's default VFS. Idempotent.
pub async fn install_opfs() -> Result<()> {
    if POOL.with(|p| p.borrow().is_some()) {
        return Ok(());
    }
    // Two networks x (two databases + their journals), plus room for
    // temporary files: 16 slots. Each slot is an OPFS file.
    let cfg = OpfsSAHPoolCfgBuilder::new().directory(OPFS_DIR).initial_capacity(16).build();
    let util = install::<sqlite_wasm_rs::WasmOsCallback>(&cfg, true)
        .await
        .map_err(|e| anyhow!("OPFS storage: {e}"))?;
    POOL.with(|p| *p.borrow_mut() = Some(util));
    Ok(())
}

fn with_pool<T>(f: impl FnOnce(&OpfsSAHPoolUtil) -> Result<T>) -> Result<T> {
    POOL.with(|p| match p.borrow().as_ref() {
        Some(u) => f(u),
        None => Err(anyhow!("OPFS storage is not installed")),
    })
}

pub fn wallet_path(net: &str) -> String {
    format!("/{net}/wallet.sqlite")
}

pub fn leek_path(net: &str) -> String {
    format!("/{net}/leek.sqlite")
}

/// Every file in the pool (databases and journals).
pub fn files() -> Result<Vec<String>> {
    use sqlite_wasm_rs::vfs::VfsFilesManager;
    with_pool(|u| u.names().map_err(|e| anyhow!("{e}")))
}

/// Whether a wallet database exists for `net`.
pub fn wallet_exists(net: &str) -> Result<bool> {
    use sqlite_wasm_rs::vfs::VfsFilesManager;
    with_pool(|u| u.contains(&wallet_path(net)).map_err(|e| anyhow!("{e}")))
}

/// Deletes every file of `net`. Its connections must be closed.
pub fn remove_network(net: &str) -> Result<usize> {
    use sqlite_wasm_rs::vfs::VfsFilesManager;
    let prefix = format!("/{net}/");
    with_pool(|u| {
        let mut n = 0;
        for name in u.names().map_err(|e| anyhow!("{e}"))? {
            if name.starts_with(&prefix) && u.remove(&name).map_err(|e| anyhow!("{e}"))? {
                n += 1;
            }
        }
        Ok(n)
    })
}

/// Opens (creating) a database in the pool.
pub fn open(path: &str) -> Result<Connection> {
    let conn = Connection::open(path).with_context(|| format!("opening {path}"))?;
    // Temporary tables in memory: no pool slots, no OPFS writes.
    conn.pragma_update(None, "temp_store", "MEMORY")?;
    Ok(conn)
}

/// Size of a database in bytes.
pub fn db_bytes(conn: &Connection) -> Result<u64> {
    let pages: i64 = conn.query_row("PRAGMA page_count", [], |r| r.get(0))?;
    let size: i64 = conn.query_row("PRAGMA page_size", [], |r| r.get(0))?;
    Ok((pages * size) as u64)
}

/// The extension's own tables beside the wallet database.
pub fn open_leek(net: &str) -> Result<Connection> {
    let conn = open(&leek_path(net))?;
    conn.execute_batch(
        "CREATE TABLE IF NOT EXISTS pending (
             view_sha256 BLOB PRIMARY KEY,   -- SHA-256 of the signer view
             account_uuid TEXT NOT NULL,
             created_ms INTEGER NOT NULL,
             summary TEXT NOT NULL,          -- the proposal summary, JSON
             signer_view BLOB NOT NULL,      -- what the signer was handed
             full_pczt BLOB NOT NULL,        -- what was kept for proving
             signed_pczt BLOB,               -- after the .zsig was applied
             proven_pczt BLOB,               -- after proving
             txid TEXT                       -- after extraction
         );
         CREATE TABLE IF NOT EXISTS sent (
             txid TEXT PRIMARY KEY,
             raw BLOB NOT NULL,
             created_ms INTEGER NOT NULL,
             broadcast_ms INTEGER,
             broadcast_error TEXT
         );",
    )?;
    Ok(conn)
}
