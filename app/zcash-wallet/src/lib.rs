//! The Zcash LeekWallet app's wallet layer, over `zecz` (host/zecz-core).
//!
//! What the app's wallet commands do, without Tauri: blocking functions
//! that take a wallet's directory (a [`zecz::Profile`]) and return plain,
//! serializable data. The commands (app/src-tauri/src/zwallet.rs) run them
//! on worker threads; the tests call them directly.
//!
//! - [`backend`]: status, imports (from the board, or a bare viewing key),
//!   sync, balances, addresses, history.
//! - [`hardware`]: sends and shieldings proposed for the board to sign, and
//!   finished with the `.zsig` it returns. The board itself is the
//!   webview's: it holds the paired, encrypted session.
//! - [`keys`]: which wallet a command is about, and where its files live
//!   (one wallet per network and board seed and account, or per viewing
//!   key), with the older one-wallet-per-network layout settled into it.
//! - [`contacts`], [`settings`]: the address books and the saved servers.
//! - [`migrate`]: the first start after the old desktop app, which copies
//!   its wallets, contacts and swaps into this app's layout.
//!
//! Nothing here holds a spending key: the wallets are viewing keys, and
//! every spend is signed on the board.

pub mod backend;
pub mod contacts;
pub mod hardware;
pub mod keys;
pub mod migrate;
pub mod settings;
