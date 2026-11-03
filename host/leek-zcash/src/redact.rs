//! The bytes that go to the board: upstream's signer view.

use pczt::Pczt;
pub use zcash_client_backend::data_api::wallet::SignerView;
use zcash_client_backend::data_api::wallet::redact_pczt_for_signer;

use crate::error::LeekError;

/// The signer view of `pczt`, serialized: what to send to the board (over
/// a link, or on an SD card). `zcash_client_backend`'s
/// `redact_pczt_for_signer`: wallet metadata, witnesses and dummy keys are
/// dropped; [`SignerView::Compact`] also drops proofs, binding keys and the
/// fields the board recomputes, about half the bytes. Keep `pczt` itself:
/// the signatures go back into it ([`apply_zsig`](crate::apply_zsig)).
pub fn signer_view_bytes(pczt: &Pczt, view: SignerView) -> Result<Vec<u8>, LeekError> {
    redact_pczt_for_signer(pczt, view).serialize().map_err(|_| LeekError::Parse)
}
