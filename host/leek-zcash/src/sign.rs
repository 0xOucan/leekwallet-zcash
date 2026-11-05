//! Signing a PCZT on the board, end to end.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use leek_device::device::Device;
use leek_device::zsig::{Entry, Zsig};
use leek_device::FramePort;
use leek_transport_serial::TransportError;
use pczt::Pczt;

use crate::account::{get_account, get_features, seed_fingerprint, AccountMeta, Features};
use crate::apply::{apply_entries, shielded_sighash, Signature};
use crate::error::{from_device, Ctx, LeekError};
use crate::preflight::preflight;
use crate::redact::{signer_view_bytes, SignerView};

/// A cancellation flag shared between the caller and a running request.
#[derive(Clone, Debug, Default)]
pub struct CancelToken(Arc<AtomicBool>);

impl CancelToken {
    pub fn new() -> Self {
        Self::default()
    }

    /// Asks the running request to stop.
    pub fn cancel(&self) {
        self.0.store(true, Ordering::SeqCst);
    }

    pub fn is_cancelled(&self) -> bool {
        self.0.load(Ordering::SeqCst)
    }
}

/// A [`FramePort`] that a [`CancelToken`] can interrupt mid-request.
///
/// It waits for a reply in short slices and gives up between them once the
/// token is set. That needs the inner port's `recv` to keep a partial frame
/// across calls, as `leek_transport_serial::SerialTransport` does. A
/// request cut short leaves the session out of step: reconnect afterwards
/// ([`LeekError::Cancelled`] says so). Wrap the port when building the
/// [`Device`]: `Device::with_port(Box::new(CancellablePort::new(port, token)))`.
pub struct CancellablePort<P> {
    inner: P,
    token: CancelToken,
}

impl<P: FramePort> CancellablePort<P> {
    pub fn new(inner: P, token: CancelToken) -> Self {
        Self { inner, token }
    }

    fn cancelled() -> TransportError {
        TransportError::Io(std::io::Error::new(std::io::ErrorKind::Interrupted, "cancelled"))
    }
}

const SLICE: Duration = Duration::from_millis(100);

impl<P: FramePort> FramePort for CancellablePort<P> {
    fn send(&mut self, frame_type: u8, payload: &[u8]) -> Result<(), TransportError> {
        if self.token.is_cancelled() {
            return Err(Self::cancelled());
        }
        self.inner.send(frame_type, payload)
    }

    fn recv(&mut self, timeout: Duration) -> Result<(u8, Vec<u8>), TransportError> {
        let deadline = Instant::now() + timeout;
        loop {
            if self.token.is_cancelled() {
                return Err(Self::cancelled());
            }
            let left = deadline.saturating_duration_since(Instant::now());
            match self.inner.recv(left.min(SLICE)) {
                Err(TransportError::Timeout) if left > SLICE => continue,
                r => return r,
            }
        }
    }
}

/// Where [`sign_pczt`] is.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SignProgress {
    /// Checking the PCZT on the host.
    Checking,
    /// Uploading the signer view: bytes sent so far, of the total.
    Uploading { sent: usize, total: usize },
    /// Every byte is on the board: the user reviews the transaction there
    /// and accepts or rejects it (Cancel on the board stops it).
    ReviewOnDevice,
    /// Verifying and applying the board's signatures.
    Applying,
}

/// Options for [`sign_pczt`].
pub struct SignOptions<'a> {
    /// The view sent to the board. [`SignerView::Full`] by default (what
    /// every firmware accepts); [`SignerView::Compact`] is about half the
    /// bytes, worth it over Bluetooth.
    pub view: SignerView,
    /// Called at each step.
    pub progress: Option<&'a dyn Fn(SignProgress)>,
    /// Checked between steps; to interrupt an upload or a wait as well,
    /// build the device over a [`CancellablePort`] with the same token.
    pub cancel: Option<CancelToken>,
    /// The board's features, if already read; otherwise they are asked for.
    pub features: Option<Features>,
    /// Ask the board which seed it holds first ([`seed_fingerprint`], no
    /// question on the board) and refuse with [`LeekError::WrongAccount`] if
    /// it is not the one recorded for the account, before uploading
    /// anything. Firmware without that call is asked for the account, which
    /// the board answers only after "Share view key?".
    pub verify_account: bool,
}

impl Default for SignOptions<'_> {
    fn default() -> Self {
        SignOptions { view: SignerView::Full, progress: None, cancel: None, features: None, verify_account: false }
    }
}

/// What the board signed.
#[derive(Clone, Debug)]
pub struct SignResult {
    /// The wallet's PCZT with the board's signatures applied and verified:
    /// pass it to the wallet's combine / prove / extract path.
    pub signed_pczt: Pczt,
    /// The signatures, as the board returned them.
    pub signatures: Vec<Signature>,
    /// The shielded sighash the user reviewed on the board (checked equal to
    /// the host's).
    pub sighash: [u8; 32],
    /// The same signatures as a `.zsig` file, bound to the bytes sent (for
    /// audit, or to apply again with [`apply_zsig`](crate::apply_zsig)).
    pub zsig: Vec<u8>,
    /// The transaction's ID (ZIP 244: it commits to the effects, not to
    /// proofs or signatures), so it is final already, before the wallet
    /// proves and extracts. In internal byte order; [`txid_hex`](Self::txid_hex)
    /// is the form explorers and RPCs show.
    pub txid: [u8; 32],
}

impl SignResult {
    pub(crate) fn new(signed_pczt: Pczt, entries: &[Entry], sighash: [u8; 32], zsig: Vec<u8>) -> Result<Self, LeekError> {
        Ok(SignResult {
            txid: crate::persist::pczt_txid(&signed_pczt)?,
            signatures: entries.iter().map(Signature::from_entry).collect(),
            signed_pczt,
            sighash,
            zsig,
        })
    }
}

/// Signs `pczt` on the board for `account`: [`preflight`], the signer view
/// ([`signer_view_bytes`]), upload and review on the board, then
/// [`apply_zsig`](crate::apply_zsig)'s checks on what comes back (the board's
/// sighash must also be the host's). `pczt` is the wallet's full copy; run
/// [`prepare_for_signer`](crate::prepare_for_signer) first if its builder
/// leaves derivations out.
///
/// The device must be paired and unlocked (`Device::pair`,
/// `Device::ensure_unlocked`).
pub fn sign_pczt(
    device: &mut Device,
    pczt: &Pczt,
    account: &AccountMeta,
    opts: SignOptions<'_>,
) -> Result<SignResult, LeekError> {
    let progress = |p| {
        if let Some(f) = opts.progress {
            f(p)
        }
    };
    let cancelled = || opts.cancel.as_ref().is_some_and(CancelToken::is_cancelled);
    let check = || if cancelled() { Err(LeekError::Cancelled) } else { Ok(()) };
    let dev_err = |e: anyhow::Error, ctx| if cancelled() { LeekError::Cancelled } else { from_device(&e, ctx) };

    progress(SignProgress::Checking);
    let features = match opts.features {
        Some(f) => f,
        None => get_features(device).map_err(|e| if cancelled() { LeekError::Cancelled } else { e })?,
    };
    preflight(pczt, account, &features)?;
    check()?;
    if opts.verify_account {
        let fp = match seed_fingerprint(device, Some(account.network)) {
            Err(LeekError::FirmwareTooOld) => get_account(device, account.index, account.network).map(|a| a.seed_fingerprint),
            r => r,
        }
        .map_err(|e| if cancelled() { LeekError::Cancelled } else { e })?;
        if fp != account.seed_fingerprint {
            return Err(LeekError::WrongAccount);
        }
        check()?;
    }

    let sent = signer_view_bytes(pczt, opts.view)?;
    if sent.len() > features.max_pczt_bytes {
        return Err(LeekError::TooLarge);
    }
    let host_sighash = shielded_sighash(pczt.clone())?;
    check()?;

    let (sighash, entries) = device
        .z_sign(&sent, account.index, account.network.as_str(), |off, total| {
            progress(SignProgress::Uploading { sent: off, total });
            if off == total {
                progress(SignProgress::ReviewOnDevice);
            }
        })
        .map_err(|e| dev_err(e, Ctx::Sign))?;
    // Signed, but the caller no longer wants it: drop the signatures.
    check()?;

    progress(SignProgress::Applying);
    if sighash != host_sighash {
        return Err(LeekError::SighashMismatch);
    }
    let signed_pczt = apply_entries(pczt, &sent, &entries)?;
    let zsig = Zsig::new(&sent, entries.clone()).to_bytes().map_err(|_| LeekError::TooMany)?;
    SignResult::new(signed_pczt, &entries, sighash, zsig)
}
