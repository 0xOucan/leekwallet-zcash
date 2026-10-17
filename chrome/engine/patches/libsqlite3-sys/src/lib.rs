//! libsqlite3-sys 0.35's API, backed by sqlite-wasm-rs. See Cargo.toml.

#[cfg(target_arch = "wasm32")]
pub use sqlite_wasm_rs::*;

#[cfg(not(target_arch = "wasm32"))]
compile_error!("the libsqlite3-sys stand-in (chrome/engine/patches) is for wasm32 only");
