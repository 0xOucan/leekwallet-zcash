//! lightwalletd over gRPC-web, and the in-memory compact block cache
//! `sync::run` downloads into.
//!
//! The cache is `host/zecz-core/src/lwd.rs`'s, with progress reported as
//! each batch is replaced (see `insert`), plus a cancel flag: the
//! worker can stop a sync between two batches, and the next sync carries on
//! from what the database recorded as scanned.

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;

use async_trait::async_trait;
use zcash_client_backend::data_api::chain::{error::Error as ChainError, BlockCache, BlockSource};
use zcash_client_backend::data_api::scanning::ScanRange;
use zcash_client_backend::proto::compact_formats::CompactBlock;
use zcash_client_backend::proto::service::compact_tx_streamer_client::CompactTxStreamerClient;
use zcash_protocol::consensus::BlockHeight;

pub type Client = CompactTxStreamerClient<tonic_web_wasm_client::Client>;

/// A gRPC-web client for `url` (a proxy in front of lightwalletd).
pub fn connect(url: &str) -> Client {
    // Compact block batches can exceed tonic's 4 MiB default.
    CompactTxStreamerClient::new(tonic_web_wasm_client::Client::new(url.trim_end_matches('/').to_string()))
        .max_decoding_message_size(64 << 20)
}

/// Set by `cancel_sync` (any time, from JS); read by the cache between
/// batches. A static rather than a field: the sync holds the engine
/// borrowed while it runs.
static CANCEL: AtomicBool = AtomicBool::new(false);

pub fn request_cancel() {
    CANCEL.store(true, Ordering::SeqCst);
}

pub fn clear_cancel() {
    CANCEL.store(false, Ordering::SeqCst);
}

pub fn cancel_requested() -> bool {
    CANCEL.load(Ordering::SeqCst)
}

#[derive(Debug)]
pub struct CacheError(pub String);

impl std::fmt::Display for CacheError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "block cache: {}", self.0)
    }
}

impl std::error::Error for CacheError {}

pub const CANCELLED: &str = "sync cancelled";

/// A range of blocks `start..end` that `sync::run` has finished scanning.
#[derive(Clone, Copy, Debug)]
pub struct ScannedRange {
    pub start: u32,
    pub end: u32,
}

#[derive(Default)]
pub struct MemBlockCache<'a> {
    blocks: Mutex<BTreeMap<u32, CompactBlock>>,
    on_scanned: Option<Box<dyn Fn(ScannedRange) + 'a>>,
}

// sync::run wants a Sync cache; the callback only ever runs on the worker
// thread that drives the sync (rayon threads do trial decryption only).
unsafe impl Sync for MemBlockCache<'_> {}
unsafe impl Send for MemBlockCache<'_> {}

impl<'a> MemBlockCache<'a> {
    pub fn with_progress(on_scanned: impl Fn(ScannedRange) + 'a) -> Self {
        MemBlockCache { on_scanned: Some(Box::new(on_scanned)), ..Default::default() }
    }

    /// Reports the blocks held (the batch scanned last) as scanned and drops
    /// them. Called before each new batch.
    pub fn flush(&self) {
        let range = {
            let mut blocks = self.blocks.lock().unwrap();
            let r = match (blocks.keys().next(), blocks.keys().next_back()) {
                (Some(&s), Some(&e)) => Some(ScannedRange { start: s, end: e + 1 }),
                _ => None,
            };
            blocks.clear();
            r
        };
        if let (Some(r), Some(f)) = (range, &self.on_scanned) {
            f(r);
        }
    }
}

impl BlockSource for MemBlockCache<'_> {
    type Error = CacheError;

    fn with_blocks<F, WalletErrT>(
        &self,
        from_height: Option<BlockHeight>,
        limit: Option<usize>,
        mut with_block: F,
    ) -> Result<(), ChainError<WalletErrT, Self::Error>>
    where
        F: FnMut(CompactBlock) -> Result<(), ChainError<WalletErrT, Self::Error>>,
    {
        let start = from_height.map(u32::from).unwrap_or(0);
        let blocks: Vec<CompactBlock> = self
            .blocks
            .lock()
            .unwrap()
            .range(start..)
            .take(limit.unwrap_or(usize::MAX))
            .map(|(_, b)| b.clone())
            .collect();
        for b in blocks {
            with_block(b)?;
        }
        Ok(())
    }
}

#[async_trait]
impl BlockCache for MemBlockCache<'_> {
    fn get_tip_height(&self, range: Option<&ScanRange>) -> Result<Option<BlockHeight>, Self::Error> {
        let blocks = self.blocks.lock().unwrap();
        let tip = match range {
            Some(r) => {
                let (s, e) = (u32::from(r.block_range().start), u32::from(r.block_range().end));
                blocks.range(s..e).next_back().map(|(k, _)| *k)
            }
            None => blocks.keys().next_back().copied(),
        };
        Ok(tip.map(BlockHeight::from_u32))
    }

    async fn read(&self, range: &ScanRange) -> Result<Vec<CompactBlock>, Self::Error> {
        let (s, e) = (u32::from(range.block_range().start), u32::from(range.block_range().end));
        let blocks = self.blocks.lock().unwrap();
        let mut out = Vec::new();
        for (expected, (k, b)) in (s..).zip(blocks.range(s..e)) {
            if *k != expected {
                break;
            }
            out.push(b.clone());
        }
        Ok(out)
    }

    async fn insert(&self, compact_blocks: Vec<CompactBlock>) -> Result<(), Self::Error> {
        // The first step of every batch: a cancelled sync stops here, before
        // anything of the batch reaches the wallet.
        if cancel_requested() {
            return Err(CacheError(CANCELLED.into()));
        }
        // sync::run downloads a batch, scans it, then downloads the next, but
        // awaits its `delete`s only when the whole run ends. So a new batch
        // means the one before it has been scanned: report it and drop it
        // here, or every block of the sync would stay in memory.
        self.flush();
        let mut blocks = self.blocks.lock().unwrap();
        for b in compact_blocks {
            blocks.insert(b.height as u32, b);
        }
        Ok(())
    }

    async fn delete(&self, range: ScanRange) -> Result<(), Self::Error> {
        // Only the last batch is still here (see `insert`): report it.
        let (s, e) = (u32::from(range.block_range().start), u32::from(range.block_range().end));
        let held = {
            let mut blocks = self.blocks.lock().unwrap();
            let held = match (blocks.range(s..e).next(), blocks.range(s..e).next_back()) {
                (Some((&a, _)), Some((&b, _))) => Some(ScannedRange { start: a, end: b + 1 }),
                _ => None,
            };
            blocks.retain(|k, _| *k < s || *k >= e);
            held
        };
        if let (Some(r), Some(f)) = (held, &self.on_scanned) {
            f(r);
        }
        Ok(())
    }
}
