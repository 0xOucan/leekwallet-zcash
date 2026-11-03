//! The SD-card air gap (experimental): what the wallet writes to the card,
//! and what it does with the `.zsig` the board writes back.
//!
//! The board's side is `firmware/src/sd-sign.c`: it reads
//! [`SD_UNSIGNED_PCZT`], hashes it, checks it exactly as a USB upload, shows
//! the same review pages, hashes the file again before signing (a card that
//! changed under the review is not signed), and writes [`SD_SIGNED_ZSIG`],
//! whose hash field is the SHA-256 of the file it read. It signs for the
//! account and network selected on the board, not for anything on the card.

use leek_device::zsig::{sha256, Zsig, ZsigError};
use pczt::Pczt;

use crate::account::{AccountMeta, Features, Network};
use crate::apply::{apply_entries, shielded_sighash};
use crate::error::LeekError;
use crate::preflight::preflight;
use crate::redact::{signer_view_bytes, SignerView};
use crate::sign::SignResult;

/// The directory the board reads and writes, at the card's root.
pub const SD_DIR: &str = "LEEKZ";
/// The file the board signs: the signer view, written by the wallet.
pub const SD_UNSIGNED_PCZT: &str = "LEEKZ/unsigned.pczt";
/// The file the board writes after the user accepts.
pub const SD_SIGNED_ZSIG: &str = "LEEKZ/signed.zsig";

/// What to write to the card for the board ([`prepare_sd_sign`]).
///
/// Keep it (or at least [`bytes`](Self::bytes)) with the wallet's PCZT until
/// the `.zsig` comes back: [`finish_sd_sign`] needs the exact bytes sent.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SdRequest {
    /// The path on the card, from its root: [`SD_UNSIGNED_PCZT`].
    pub file_name: &'static str,
    /// The file's contents: the signer view of the wallet's PCZT.
    pub bytes: Vec<u8>,
    /// SHA-256 of [`bytes`](Self::bytes): the hash the board's `.zsig` must
    /// name. Show a prefix of it if the wallet wants the user to match files.
    pub sent_hash: [u8; 32],
    /// The account the board must have selected when it signs.
    pub account: u32,
    /// The network the board must be set to.
    pub network: Network,
}

impl AsRef<[u8]> for SdRequest {
    fn as_ref(&self) -> &[u8] {
        &self.bytes
    }
}

/// **Experimental.** The file to put on the SD card so the board signs
/// `pczt` for `account` without any link to the host.
///
/// Runs [`preflight`] against the current firmware's features
/// ([`Features::default`]; pass the board's own to [`preflight`] first if the
/// wallet has read them), makes the signer view ([`signer_view_bytes`]) and
/// checks it fits the board. [`SignerView::Full`] is what every firmware
/// with the SD menu reads; [`SignerView::Compact`] is about half the size.
///
/// Write [`SdRequest::bytes`] to [`SdRequest::file_name`] (creating
/// [`SD_DIR`]), and remove any old [`SD_SIGNED_ZSIG`] so a stale file is not
/// taken for the answer. The user selects the account and network on the
/// board, then Main menu > SD Card > Sign transaction.
///
/// Experimental: the card format may still change with the firmware's SD
/// code; the request and result types will stay.
pub fn prepare_sd_sign(pczt: &Pczt, account: &AccountMeta, view: SignerView) -> Result<SdRequest, LeekError> {
    let features = Features::default();
    preflight(pczt, account, &features)?;
    let bytes = signer_view_bytes(pczt, view)?;
    if bytes.len() > features.max_pczt_bytes {
        return Err(LeekError::TooLarge);
    }
    Ok(SdRequest {
        file_name: SD_UNSIGNED_PCZT,
        sent_hash: sha256(&bytes),
        bytes,
        account: account.index,
        network: account.network,
    })
}

/// **Experimental.** Applies the `.zsig` the board wrote to the card
/// ([`SD_SIGNED_ZSIG`]) to the wallet's PCZT, with every check of
/// [`apply_zsig`](crate::apply_zsig), and returns the same [`SignResult`] as
/// [`sign_pczt`](crate::sign_pczt).
///
/// - `pczt` is the wallet's own copy, the one [`prepare_sd_sign`] was given.
/// - `sent` is the [`SdRequest`] (or its bytes): what was written to the card.
/// - `zsig` is the file the board wrote.
///
/// A `.zsig` for another file (an old card, another transaction) is
/// [`LeekError::ZsigHashMismatch`].
pub fn finish_sd_sign(pczt: &Pczt, sent: impl AsRef<[u8]>, zsig: &[u8]) -> Result<SignResult, LeekError> {
    let sent = sent.as_ref();
    let z = Zsig::parse(zsig).map_err(|e| match e {
        ZsigError::HashMismatch { .. } => LeekError::ZsigHashMismatch,
        _ => LeekError::ZsigMalformed,
    })?;
    z.check_hash(sent).map_err(|_| LeekError::ZsigHashMismatch)?;
    let signed = apply_entries(pczt, sent, &z.entries)?;
    SignResult::new(signed, &z.entries, shielded_sighash(pczt.clone())?, zsig.to_vec())
}
