//! Desktop entry point; everything is in the library.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    leekwallet_zcash_lib::run()
}
