//! Bluetooth LE on the desktop: LeekWallet's btleplug transport
//! (`app/transport-ble`, feature `ble`) as a blocking [`FramePort`].
//!
//! No second Bluetooth stack: the scan, the GATT link, the chunking and the
//! UUIDs are `leek-transport-ble`'s, the ones the Zcash LeekWallet app uses.
//! This module only makes that async transport usable from `Device`, which
//! is synchronous, and adds what a command line needs on top: picking one
//! board out of a scan, and saying what to do when none answers.
//!
//! The transport lives on a thread of its own, with a small runtime that
//! keeps btleplug's background tasks (BlueZ's D-Bus streams) running, and
//! the caller exchanges frames with it over channels. So a [`BlePort`] works
//! from any thread, including inside another tokio runtime (zecz's
//! `#[tokio::main]`), where blocking on a second runtime would panic.
//!
//! Everything above the frame is the same as over USB: the six-digit
//! pairing code on every connection, the same encrypted session, the board's
//! own prompts (PROTOCOL.md §2: chunking is the only thing BLE adds).

use std::sync::mpsc as std_mpsc;
use std::time::Duration;

use anyhow::{anyhow, bail, Result};
use leek_transport_ble::{BleError, BleTransport};
use leek_transport_serial::TransportError;
use tokio::sync::mpsc;

pub use leek_transport_ble::{BleDevice, SERVICE_UUID};

use crate::device::{Device, FramePort};

/// How long a scan looks, as in the Zcash LeekWallet app.
pub const DEFAULT_SCAN: Duration = Duration::from_millis(4000);

/// Where the board's Bluetooth link is switched on. The board advertises on
/// one link only (PROTOCOL.md §3b).
pub const BOARD_LINK_HINT: &str = "On the board: Settings → Link → Bluetooth.";

/// List the LeekWallet boards advertising the service (by service UUID,
/// never by name), waiting out the whole window so a second board is not
/// hidden behind the first.
pub fn scan(window: Duration) -> Result<Vec<BleDevice>> {
    on_ble_thread(move || async move { BleTransport::scan(window).await }).map_err(explain)
}

/// Which board to connect to: the one `want` names (its address, as `scan`
/// lists it, case ignored), or the only one found. More than one and no
/// `want` is an error that lists them: connecting to "the first" could sign
/// on a board the user did not mean.
pub fn pick<'a>(found: &'a [BleDevice], want: Option<&str>) -> Result<&'a BleDevice> {
    if let Some(want) = want {
        return found
            .iter()
            .find(|d| d.id.eq_ignore_ascii_case(want.trim()))
            .ok_or_else(|| anyhow!("no LeekWallet with address {want} is advertising. {BOARD_LINK_HINT}{}", list_suffix(found)));
    }
    match found {
        [] => bail!("no LeekWallet board is advertising over Bluetooth. {BOARD_LINK_HINT} Then keep it on and near this computer."),
        [one] => Ok(one),
        many => bail!("{} LeekWallet boards are advertising; choose one with --ble-addr:\n{}", many.len(), list(many)),
    }
}

/// One line per board: address and name.
pub fn list(found: &[BleDevice]) -> String {
    found.iter().map(describe).collect::<Vec<_>>().join("\n")
}

/// "  28:84:85:9F:D2:0E  LeekWallet" (the name is decoration; the address
/// is what `--ble-addr` takes).
pub fn describe(d: &BleDevice) -> String {
    format!("  {}  {}", d.id, d.name.as_deref().unwrap_or("(no name)"))
}

fn list_suffix(found: &[BleDevice]) -> String {
    if found.is_empty() {
        String::new()
    } else {
        format!(" Advertising now:\n{}", list(found))
    }
}

/// A transport error as a sentence with what to do about it: the adapter,
/// a Bluetooth permission, or the board's link setting.
pub fn explain(e: BleError) -> anyhow::Error {
    let text = e.to_string();
    let hint = match &e {
        BleError::NoAdapter => {
            "No Bluetooth adapter on this computer (Linux: `bluetoothctl show` should list one, powered). \
             Use --port for USB instead.".to_string()
        }
        BleError::AdapterOff => "Bluetooth is switched off on this computer. Turn it on (`bluetoothctl power on`) and try again.".to_string(),
        BleError::PermissionDenied => permission_hint(),
        BleError::Ble(s) if looks_like_permission(s) => format!("{} ({s})", permission_hint()),
        BleError::Ble(s) if looks_like_no_bluez(s) => format!(
            "Bluetooth is not available: is the bluetooth service running (`systemctl status bluetooth`)? ({s})"
        ),
        BleError::NotFound { .. } | BleError::NotAdvertising { .. } => {
            format!("{text}\n{BOARD_LINK_HINT} Pressing a button on the board makes it advertise again.")
        }
        _ => text,
    };
    anyhow!(hint)
}

fn permission_hint() -> String {
    "This user may not use Bluetooth. Linux: BlueZ must allow you over D-Bus (a desktop session, or the \
     `bluetooth` group on some systems); macOS: allow Bluetooth for your terminal in System Settings → \
     Privacy & Security → Bluetooth."
        .to_string()
}

fn looks_like_permission(s: &str) -> bool {
    let s = s.to_ascii_lowercase();
    ["notauthorized", "not authorized", "accessdenied", "access denied", "permission"].iter().any(|k| s.contains(k))
}

fn looks_like_no_bluez(s: &str) -> bool {
    let s = s.to_ascii_lowercase();
    s.contains("org.bluez") && (s.contains("serviceunknown") || s.contains("was not provided"))
}

/// Run one async btleplug job on a thread with its own runtime, from any
/// context. Everything btleplug made is dropped inside that runtime: BlueZ's
/// event streams spawn a cleanup task on drop, which panics outside one.
fn on_ble_thread<F, Fut, T>(job: F) -> Result<T, BleError>
where
    F: FnOnce() -> Fut + Send + 'static,
    Fut: std::future::Future<Output = Result<T, BleError>>,
    T: Send + 'static,
{
    std::thread::Builder::new()
        .name("leek-ble".into())
        .spawn(move || -> Result<T, BleError> {
            let rt = runtime()?;
            rt.block_on(job())
        })
        .map_err(|e| BleError::Ble(format!("cannot start the Bluetooth thread: {e}")))?
        .join()
        .map_err(|_| BleError::Ble("the Bluetooth thread panicked".into()))?
}

fn runtime() -> Result<tokio::runtime::Runtime, BleError> {
    // A worker that runs between frames: btleplug keeps background tasks
    // for the notification stream.
    tokio::runtime::Builder::new_multi_thread()
        .worker_threads(1)
        .thread_name("leek-ble-rt")
        .enable_all()
        .build()
        .map_err(|e| BleError::Ble(format!("Bluetooth runtime: {e}")))
}

enum Job {
    Send(u8, Vec<u8>, std_mpsc::SyncSender<Result<(), BleError>>),
    Recv(Duration, std_mpsc::SyncSender<Result<(u8, Vec<u8>), BleError>>),
}

/// leek-transport-ble as a blocking [`FramePort`]. Dropping it disconnects
/// explicitly (unsubscribe, then the link), so the board, which serves one
/// peer at a time, is free for the next connection at once.
pub struct BlePort {
    jobs: Option<mpsc::UnboundedSender<Job>>,
    thread: Option<std::thread::JoinHandle<()>>,
    id: String,
}

/// How long the disconnect may take when the port is dropped.
const CLOSE_WAIT: Duration = Duration::from_secs(5);

impl BlePort {
    /// Connect to the board with this address (from [`scan`]).
    pub fn connect_id(id: &str, window: Duration) -> Result<Self> {
        let (ready_tx, ready_rx) = std_mpsc::sync_channel::<Result<(), BleError>>(1);
        let (jobs_tx, mut jobs_rx) = mpsc::unbounded_channel::<Job>();
        let want = id.to_string();
        let thread = std::thread::Builder::new()
            .name("leek-ble".into())
            .spawn(move || {
                let rt = match runtime() {
                    Ok(rt) => rt,
                    Err(e) => {
                        let _ = ready_tx.send(Err(e));
                        return;
                    }
                };
                rt.block_on(async move {
                    let mut t = match BleTransport::connect_id(&want, window).await {
                        Ok(t) => t,
                        Err(e) => {
                            let _ = ready_tx.send(Err(e));
                            return;
                        }
                    };
                    // The MTU stays at the 23-byte floor: btleplug cannot read
                    // the negotiated one, and a larger guess truncates.
                    let _ = ready_tx.send(Ok(()));
                    // Until the port is dropped (the sender goes away).
                    while let Some(job) = jobs_rx.recv().await {
                        match job {
                            Job::Send(ft, payload, reply) => {
                                let _ = reply.send(t.send(ft, &payload).await);
                            }
                            Job::Recv(timeout, reply) => {
                                let _ = reply.send(t.recv(timeout).await);
                            }
                        }
                    }
                    let _ = tokio::time::timeout(CLOSE_WAIT, t.disconnect()).await;
                    drop(t);
                });
            })
            .map_err(|e| anyhow!("cannot start the Bluetooth thread: {e}"))?;
        match ready_rx.recv() {
            Ok(Ok(())) => Ok(Self { jobs: Some(jobs_tx), thread: Some(thread), id: id.to_string() }),
            Ok(Err(e)) => {
                let _ = thread.join();
                Err(explain(e))
            }
            Err(_) => {
                let _ = thread.join();
                bail!("the Bluetooth thread stopped before connecting")
            }
        }
    }

    /// The board's address.
    pub fn id(&self) -> &str {
        &self.id
    }

    fn submit(&self, job: Job) -> Result<(), TransportError> {
        self.jobs
            .as_ref()
            .ok_or_else(closed)?
            .send(job)
            .map_err(|_| closed())
    }
}

fn closed() -> TransportError {
    TransportError::Io(std::io::Error::other("the Bluetooth link is closed"))
}

/// `Timeout` stays a timeout, so leek-device can say the device did not
/// answer; anything else keeps the transport's own sentence.
fn to_transport(e: BleError) -> TransportError {
    match e {
        BleError::Timeout => TransportError::Timeout,
        e => TransportError::Io(std::io::Error::other(e.to_string())),
    }
}

impl FramePort for BlePort {
    fn send(&mut self, frame_type: u8, payload: &[u8]) -> Result<(), TransportError> {
        let (tx, rx) = std_mpsc::sync_channel(1);
        self.submit(Job::Send(frame_type, payload.to_vec(), tx))?;
        rx.recv().map_err(|_| closed())?.map_err(to_transport)
    }

    fn recv(&mut self, timeout: Duration) -> Result<(u8, Vec<u8>), TransportError> {
        let (tx, rx) = std_mpsc::sync_channel(1);
        self.submit(Job::Recv(timeout, tx))?;
        rx.recv().map_err(|_| closed())?.map_err(to_transport)
    }
}

impl Drop for BlePort {
    fn drop(&mut self) {
        // Closing the channel ends the thread's loop, which disconnects.
        drop(self.jobs.take());
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

impl Device {
    /// The device over Bluetooth LE, at the address [`scan`] reported.
    /// Pair it as over USB ([`Device::pair`]): the six-digit code every time.
    pub fn open_ble(id: &str, window: Duration) -> Result<Self> {
        Ok(Self::with_port(Box::new(BlePort::connect_id(id, window)?)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn board(id: &str, name: Option<&str>) -> BleDevice {
        BleDevice { id: id.into(), name: name.map(Into::into) }
    }

    #[test]
    fn picks_the_only_board() {
        let found = [board("28:84:85:9F:D2:0E", Some("LeekWallet"))];
        assert_eq!(pick(&found, None).unwrap().id, "28:84:85:9F:D2:0E");
    }

    #[test]
    fn none_found_says_where_the_setting_is() {
        let e = pick(&[], None).unwrap_err().to_string();
        assert!(e.contains("Settings → Link → Bluetooth"), "{e}");
    }

    #[test]
    fn two_boards_need_an_address() {
        let found = [board("AA:AA:AA:AA:AA:01", Some("LeekWallet")), board("AA:AA:AA:AA:AA:02", None)];
        let e = pick(&found, None).unwrap_err().to_string();
        assert!(e.contains("--ble-addr"), "{e}");
        assert!(e.contains("AA:AA:AA:AA:AA:01  LeekWallet"), "{e}");
        assert!(e.contains("AA:AA:AA:AA:AA:02  (no name)"), "{e}");
    }

    #[test]
    fn address_matches_case_insensitively() {
        let found = [board("AA:AA:AA:AA:AA:01", None), board("AA:AA:AA:AA:AA:02", None)];
        assert_eq!(pick(&found, Some(" aa:aa:aa:aa:aa:02 ")).unwrap().id, "AA:AA:AA:AA:AA:02");
    }

    #[test]
    fn unknown_address_lists_what_is_there() {
        let found = [board("AA:AA:AA:AA:AA:01", Some("LeekWallet"))];
        let e = pick(&found, Some("BB:BB:BB:BB:BB:BB")).unwrap_err().to_string();
        assert!(e.contains("BB:BB:BB:BB:BB:BB"), "{e}");
        assert!(e.contains("AA:AA:AA:AA:AA:01"), "{e}");
        assert!(e.contains("Settings → Link → Bluetooth"), "{e}");
    }

    #[test]
    fn errors_say_what_to_do() {
        assert!(explain(BleError::NoAdapter).to_string().contains("No Bluetooth adapter"));
        assert!(explain(BleError::AdapterOff).to_string().contains("switched off"));
        assert!(explain(BleError::PermissionDenied).to_string().contains("may not use Bluetooth"));
        let dbus = BleError::Ble("org.freedesktop.DBus.Error.AccessDenied: rejected send".into());
        assert!(explain(dbus).to_string().contains("may not use Bluetooth"));
        let nobluez = BleError::Ble("The name org.bluez was not provided by any .service files".into());
        assert!(explain(nobluez).to_string().contains("systemctl status bluetooth"));
        let gone = explain(BleError::NotFound { scanned_for: Duration::from_secs(4) }).to_string();
        assert!(gone.contains("Settings → Link → Bluetooth"), "{gone}");
        assert!(explain(BleError::Timeout).to_string().contains("timed out"));
    }

    #[test]
    fn timeout_stays_a_timeout() {
        assert!(matches!(to_transport(BleError::Timeout), TransportError::Timeout));
        assert!(matches!(to_transport(BleError::NoAdapter), TransportError::Io(_)));
    }
}
