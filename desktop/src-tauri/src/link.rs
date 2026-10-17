//! The links to a LeekWallet: USB serial and Bluetooth LE.
//!
//! `zecz::device::Device` speaks the protocol over any `FramePort`. The
//! serial port already is one (leek-transport-serial); Bluetooth is
//! LeekWallet's btleplug transport (leek-transport-ble), which is async, so
//! [`BlePort`] runs it on a small runtime of its own and blocks on each
//! frame. Everything above the frame is the same on both links
//! (PROTOCOL.md §2: chunking is the only thing BLE adds).

use std::time::Duration;

use anyhow::{anyhow, Result};
use leek_transport_ble::{BleError, BleTransport};
use leek_transport_serial::TransportError;
use serde::Serialize;
use zecz::device::{Device, FramePort};

/// How long a Bluetooth scan looks, as in LeekWallet's own app.
pub const BLE_SCAN: Duration = Duration::from_millis(4000);

/// A serial port, as the connect dialog lists it.
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PortDto {
    pub name: String,
    pub description: String,
    /// Espressif's USB vendor ID (0x303A): most likely the board.
    pub likely_device: bool,
}

/// A device advertising the LeekWallet service.
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct BleDeviceDto {
    pub id: String,
    pub name: Option<String>,
}

/// USB serial ports, the board's (VID 303a) first. Ports of other USB
/// devices stay listed after it: a board behind a hub or an adapter may
/// report another vendor.
pub fn list_ports() -> Result<Vec<PortDto>> {
    let ports = leek_transport_serial::list_ports().map_err(|e| anyhow!("cannot list serial ports: {e}"))?;
    Ok(ports
        .into_iter()
        .map(|p| PortDto { name: p.name, description: p.description, likely_device: p.likely_device })
        .collect())
}

/// Opens the device on a serial port (the board, or zdev's PTY).
pub fn open_usb(port: &str) -> Result<Device> {
    if port.trim().is_empty() {
        return Err(anyhow!("choose the board's serial port"));
    }
    Device::open(port).map_err(|e| {
        anyhow!("{e:#}. Check that the board is plugged in with a data cable, that USB is its selected link, and that no other app has the port open.")
    })
}

/// Lists LeekWallets advertising over Bluetooth.
pub fn ble_scan() -> Result<Vec<BleDeviceDto>> {
    let rt = ble_runtime()?;
    let found = rt.block_on(BleTransport::scan(BLE_SCAN)).map_err(ble_err)?;
    Ok(found.into_iter().map(|d| BleDeviceDto { id: d.id, name: d.name }).collect())
}

/// Connects over Bluetooth: to `id` from [`ble_scan`], or to the first
/// LeekWallet found.
pub fn open_ble(id: Option<&str>) -> Result<Device> {
    Ok(Device::with_port(Box::new(BlePort::connect(id)?)))
}

fn ble_runtime() -> Result<tokio::runtime::Runtime> {
    // btleplug (BlueZ over D-Bus on Linux) keeps background tasks for the
    // notification stream, so they need a worker that runs between frames.
    tokio::runtime::Builder::new_multi_thread()
        .worker_threads(1)
        .thread_name("leekz-ble")
        .enable_all()
        .build()
        .map_err(|e| anyhow!("bluetooth runtime: {e}"))
}

fn ble_err(e: BleError) -> anyhow::Error {
    anyhow!("{e}")
}

/// leek-transport-ble as a blocking `FramePort`.
pub struct BlePort {
    /// Both are Some until the port is dropped.
    rt: Option<tokio::runtime::Runtime>,
    transport: Option<BleTransport>,
}

impl BlePort {
    pub fn connect(id: Option<&str>) -> Result<Self> {
        let rt = ble_runtime()?;
        let transport = rt
            .block_on(async {
                match id {
                    Some(id) => BleTransport::connect_id(id, BLE_SCAN).await,
                    None => BleTransport::connect(BLE_SCAN).await,
                }
            })
            .map_err(ble_err)?;
        // The MTU stays at the 23-byte floor: btleplug cannot read the
        // negotiated one, and a larger guess truncates (PROTOCOL.md §2).
        Ok(BlePort { rt: Some(rt), transport: Some(transport) })
    }

    /// The runtime's handle and the transport, borrowed together.
    fn parts(&mut self) -> Result<(tokio::runtime::Handle, &mut BleTransport), TransportError> {
        match (&self.rt, self.transport.as_mut()) {
            (Some(rt), Some(t)) => Ok((rt.handle().clone(), t)),
            _ => Err(TransportError::Io(std::io::Error::other("the Bluetooth link is closed"))),
        }
    }
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
        let (rt, t) = self.parts()?;
        rt.block_on(t.send(frame_type, payload)).map_err(to_transport)
    }

    fn recv(&mut self, timeout: Duration) -> Result<(u8, Vec<u8>), TransportError> {
        let (rt, t) = self.parts()?;
        rt.block_on(t.recv(timeout)).map_err(to_transport)
    }
}

impl Drop for BlePort {
    /// An explicit disconnect, never just a drop: LeekWallet's transport
    /// unsubscribes first, or BlueZ keeps a stale subscription and the next
    /// connection hears nothing; and the device serves one peer at a time.
    fn drop(&mut self) {
        let (Some(rt), Some(mut t)) = (self.rt.take(), self.transport.take()) else { return };
        let mut close = move || {
            let _ = rt.block_on(async { tokio::time::timeout(Duration::from_secs(5), t.disconnect()).await });
        };
        // A runtime can be neither blocked on nor dropped from inside
        // another one; the app drops sessions on worker threads, but if this
        // ever runs on an async thread, close the link from a thread of its own.
        if tokio::runtime::Handle::try_current().is_ok() {
            let _ = std::thread::Builder::new().name("leekz-ble-close".into()).spawn(close);
        } else {
            close();
        }
    }
}
