//! wasm-bindgen surface, timed from JavaScript (bench/).

use wasm_bindgen::prelude::*;

#[cfg(feature = "threads")]
pub use wasm_bindgen_rayon::init_thread_pool;

#[wasm_bindgen]
pub struct Prover {
    pk: orchard::circuit::ProvingKey,
}

#[wasm_bindgen]
impl Prover {
    /// Builds the NU6.3 Orchard/Ironwood proving key (nothing to download:
    /// Halo 2 has no trusted setup).
    #[wasm_bindgen(constructor)]
    pub fn new() -> Prover {
        Prover { pk: crate::prove::build_pk() }
    }

    /// Proves a bundle with `outputs` outputs; returns the proof length in bytes.
    pub fn prove(&self, outputs: usize) -> usize {
        crate::prove::prove(&self.pk, outputs).1.len()
    }
}

#[wasm_bindgen]
pub fn threads_enabled() -> bool {
    cfg!(feature = "threads")
}

/// Size of the WASM linear memory (it only grows): the engine's peak heap.
#[wasm_bindgen]
pub fn wasm_memory_mb() -> f64 {
    let mem: js_sys::WebAssembly::Memory = wasm_bindgen::memory().unchecked_into();
    let buf: js_sys::Object = mem.buffer().unchecked_into();
    js_sys::Reflect::get(&buf, &"byteLength".into()).unwrap().as_f64().unwrap() / 1e6
}

#[cfg(feature = "engine")]
mod engine_web {
    use super::*;
    use crate::engine;
    use zcash_client_backend::proto::service::{
        compact_tx_streamer_client::CompactTxStreamerClient, BlockId, BlockRange, ChainSpec, Empty,
    };

    fn now() -> f64 {
        js_sys::Date::now()
    }

    fn client(url: &str) -> CompactTxStreamerClient<tonic_web_wasm_client::Client> {
        CompactTxStreamerClient::new(tonic_web_wasm_client::Client::new(url.to_string()))
    }

    fn js(e: impl std::fmt::Display) -> JsError {
        JsError::new(&e.to_string())
    }

    #[wasm_bindgen]
    pub struct Verifier {
        vk: orchard::circuit::VerifyingKey,
    }

    #[wasm_bindgen]
    impl Verifier {
        #[wasm_bindgen(constructor)]
        pub fn new() -> Verifier {
            Verifier { vk: crate::prove::build_vk() }
        }
    }

    /// Board-side signing (dev soft signer), then prove + extract: JSON timings.
    #[wasm_bindgen]
    pub fn sign_prove_extract(
        prover: &Prover,
        verifier: &Verifier,
        pczt: &[u8],
        mnemonic: &str,
    ) -> Result<String, JsError> {
        let t = engine::sign_prove_extract(pczt, mnemonic, &prover.pk, &verifier.vk, now).map_err(js)?;
        Ok(format!(
            r#"{{"txid":"{}","tx_len":{},"actions":{},"sign_ms":{},"prove_ms":{},"extract_ms":{}}}"#,
            t.txid, t.tx_len, t.actions, t.sign_ms, t.prove_ms, t.extract_ms
        ))
    }

    /// GetLightdInfo + GetLatestBlock over gRPC-web.
    #[wasm_bindgen]
    pub async fn lwd_info(url: String) -> Result<String, JsError> {
        let mut c = client(&url);
        let info = c.get_lightd_info(Empty {}).await.map_err(js)?.into_inner();
        let tip = c.get_latest_block(ChainSpec {}).await.map_err(js)?.into_inner();
        Ok(format!(
            r#"{{"chain":"{}","block_height":{},"tip":{},"branch":"{}","version":"{}"}}"#,
            info.chain_name, info.block_height, tip.height, info.consensus_branch_id, info.version
        ))
    }

    /// GetBlockRange [start, start+count) over gRPC-web (server streaming),
    /// then trial-decrypt it for the mnemonic's account: JSON timings.
    #[wasm_bindgen]
    pub async fn fetch_and_scan(url: String, start: u64, count: u64, mnemonic: String) -> Result<String, JsError> {
        let ufvk = engine::ufvk_for(&mnemonic).map_err(js)?;
        let mut c = client(&url);
        let t0 = now();
        let range = BlockRange {
            start: Some(BlockId { height: start, hash: vec![] }),
            end: Some(BlockId { height: start + count - 1, hash: vec![] }),
            pool_types: vec![],
        };
        let mut stream = c.get_block_range(range).await.map_err(js)?.into_inner();
        let mut blocks = Vec::new();
        let mut bytes = 0usize;
        while let Some(b) = stream.message().await.map_err(js)? {
            bytes += prost::Message::encoded_len(&b);
            blocks.push(b);
        }
        let fetch_ms = now() - t0;
        let t0 = now();
        let s = engine::scan(blocks, &ufvk).map_err(js)?;
        let scan_ms = now() - t0;
        Ok(format!(
            r#"{{"blocks":{},"bytes":{},"orchard_ironwood_actions":{},"sapling_outputs":{},"found":{},"fetch_ms":{},"scan_ms":{}}}"#,
            s.blocks, bytes, s.actions, s.sapling_outputs, s.found, fetch_ms, scan_ms
        ))
    }
}
