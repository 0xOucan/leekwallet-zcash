//! Phase 9.1 engine spike. See README.md.

pub mod prove;

#[cfg(feature = "engine")]
pub mod engine;

#[cfg(all(feature = "sqlite", target_arch = "wasm32"))]
pub mod wallet;

#[cfg(target_arch = "wasm32")]
mod web;
