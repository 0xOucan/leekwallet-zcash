//! LeekWallet Zcash: Rust signer backend.
//!
//! no_std + alloc static library with a C ABI. The host firmware (C, ESP-IDF)
//! provides allocation, entropy and abort through the `zs_platform_*` symbols,
//! so the same crate links into the ESP32-S3 image and the host simulator.
#![no_std]

extern crate alloc;

mod platform;
pub mod account;
pub mod spike;
