//! lightwalletd access: a TLS gRPC channel and an in-memory compact block
//! cache for `zcash_client_backend::sync::run`.
//!
//! `sync::run` downloads each scan range into the cache, scans it, and
//! deletes it again, so the cache only ever holds the batch in flight. An
//! in-memory map is enough; nothing needs to survive a restart, because the
//! wallet database records what has been scanned.

use std::collections::BTreeMap;
use std::sync::Mutex;
use std::time::Duration;

use anyhow::{Context, Result};
use async_trait::async_trait;
use tonic::transport::{Channel, ClientTlsConfig, Endpoint};
use zcash_client_backend::data_api::chain::{error::Error as ChainError, BlockCache, BlockSource};
use zcash_client_backend::data_api::scanning::ScanRange;
use zcash_client_backend::proto::compact_formats::CompactBlock;
use zcash_client_backend::proto::service::compact_tx_streamer_client::CompactTxStreamerClient;
use zcash_protocol::consensus::BlockHeight;

pub type Client = CompactTxStreamerClient<Channel>;

pub async fn connect(server: &str) -> Result<Client> {
    let mut ep = Endpoint::from_shared(server.to_string())
        .with_context(|| format!("bad server URL {server}"))?
        .connect_timeout(Duration::from_secs(20))
        .timeout(Duration::from_secs(120));
    if server.starts_with("https://") {
        ep = ep.tls_config(ClientTlsConfig::new().with_webpki_roots())?;
    }
    let channel = ep.connect().await.with_context(|| format!("cannot connect to lightwalletd at {server}"))?;
    // Compact block batches can exceed tonic's 4 MiB default.
    Ok(CompactTxStreamerClient::new(channel).max_decoding_message_size(64 << 20))
}

#[derive(Debug)]
pub struct CacheError(String);

impl std::fmt::Display for CacheError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "block cache: {}", self.0)
    }
}

impl std::error::Error for CacheError {}

/// A range of blocks `start..end` that `sync::run` has finished scanning.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ScannedRange {
    pub start: u32,
    pub end: u32,
    /// The chain tip when the sync started, if known.
    pub tip: Option<u32>,
}

/// In-memory compact block cache that reports progress as ranges are
/// downloaded and scanned.
#[derive(Default)]
pub struct MemBlockCache<'a> {
    blocks: Mutex<BTreeMap<u32, CompactBlock>>,
    /// Chain tip, for progress reports only.
    pub tip: Mutex<Option<u32>>,
    on_scanned: Option<Box<dyn Fn(ScannedRange) + Send + Sync + 'a>>,
}

impl<'a> MemBlockCache<'a> {
    /// A cache that calls `on_scanned` each time a range has been scanned.
    pub fn with_progress(on_scanned: impl Fn(ScannedRange) + Send + Sync + 'a) -> Self {
        MemBlockCache { on_scanned: Some(Box::new(on_scanned)), ..Default::default() }
    }
}

fn h(b: &CompactBlock) -> u32 {
    b.height as u32
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
                break; // short, contiguous read
            }
            out.push(b.clone());
        }
        Ok(out)
    }

    async fn insert(&self, compact_blocks: Vec<CompactBlock>) -> Result<(), Self::Error> {
        let mut blocks = self.blocks.lock().unwrap();
        for b in compact_blocks {
            blocks.insert(h(&b), b);
        }
        Ok(())
    }

    async fn delete(&self, range: ScanRange) -> Result<(), Self::Error> {
        let (s, e) = (u32::from(range.block_range().start), u32::from(range.block_range().end));
        self.blocks.lock().unwrap().retain(|k, _| *k < s || *k >= e);
        // `sync::run` deletes a range once it has been scanned.
        let tip = *self.tip.lock().unwrap();
        if let Some(f) = &self.on_scanned {
            f(ScannedRange { start: s, end: e, tip });
        }
        Ok(())
    }
}
