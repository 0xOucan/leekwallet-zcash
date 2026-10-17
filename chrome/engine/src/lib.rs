//! The LeekWallet Zcash engine for the Chrome extension (roadmap 9.2).
//!
//! zecz-core's wallet flow compiled to WASM: `zcash_client_sqlite` on OPFS,
//! `sync::run` over gRPC-web, PCZT proposals, `.zsig` application, proving,
//! extraction and broadcast. It runs in a dedicated worker (threads need
//! `Atomics.wait`, see docs/zcash/CHROME-ENGINE.md) and is driven by JSON
//! messages: [`api`] documents them, `worker.js` in the extension forwards
//! them.
//!
//! What is shared with the native companions, and how:
//! - [`zsig`] is `host/leek-device/src/zsig.rs` and [`authorize`] is
//!   `host/zecz-core/src/authorize.rs`, compiled here unchanged through
//!   `#[path]`: the `.zsig` v1/v2 format and the Signer/Prover steps are
//!   the same code the CLI, desktop and Android apps run.
//! - [`keys`] and [`soft_signer`] (DEV ONLY, testnet) are zecz-core's too.
//! - [`wallet`] follows `host/zecz-core/src/wallet.rs` call for call. That
//!   file is bound to `std::fs`, tonic's HTTP/2 transport and tokio, none of
//!   which exist on wasm32, so the wallet operations are restated here over
//!   an OPFS connection and a gRPC-web client. Keep the two in step.

#![cfg(target_arch = "wasm32")]

#[path = "../../../host/leek-device/src/zsig.rs"]
pub mod zsig;

#[path = "../../../host/zecz-core/src/authorize.rs"]
pub mod authorize;

#[path = "../../../host/zecz-core/src/keys.rs"]
pub mod keys;

#[path = "../../../host/zecz-core/src/soft_signer.rs"]
pub mod soft_signer;

pub mod api;
pub mod lwd;
pub mod store;
pub mod wallet;
pub mod web;
