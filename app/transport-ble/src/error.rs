//! One error type for both radio backends.
//!
//! Shared rather than per-backend because the messages are the part that
//! matters, and a user hitting "no device found" on a phone deserves the same
//! sentence that took several confused evenings to get right on the desktop.
//! The platform stack's own error is flattened to a `String` at the boundary:
//! it is only ever displayed, and keeping it typed would drag btleplug into
//! the Android build, where the whole point is that it is not present.

use std::time::Duration;

use uuid::Uuid;

use crate::ids::SERVICE_UUID;
use crate::wire::WireError;

/// What a reconnect to a remembered board says when the board was neither
/// seen advertising nor reachable by its id: the likely cause first, then
/// what to do about each.
pub const NOT_ADVERTISING: &str =
    "The board is not advertising: it may still be connected to this or another app. \
     Restart the board or wait ~30 s. Pressing any button on the board also makes it \
     advertise again (it stops two minutes after the last disconnect), and its \
     Settings → Link must be Bluetooth.";

#[derive(Debug)]
pub enum BleError {
    /// Whatever the platform Bluetooth stack said, already formatted.
    Ble(String),
    Wire(WireError),
    /// Nothing advertising our service turned up. Carries what we looked for
    /// and for how long, because "no device found" on its own has never once
    /// helped anyone.
    NotFound { scanned_for: Duration },
    /// One remembered board, looked for by its id, did not advertise, and a
    /// direct connection to that id did not answer either. Separate from
    /// `NotFound` because the causes are different: the board is usually on,
    /// in range and on the Bluetooth link, but still connected to this app
    /// (a link the previous run never closed), to another app or phone, or
    /// past its two-minute advertising window (firmware src/ble.c,
    /// `BLE_ADV_WINDOW_MS`), which only a button press re-opens.
    NotAdvertising { scanned_for: Duration },
    NoAdapter,
    /// Connected, but the peer is not the thing we are looking for.
    MissingCharacteristic(Uuid),
    NotNotifiable(Uuid),
    Timeout,
    /// Android only: the app does not hold `BLUETOOTH_SCAN`/`BLUETOOTH_CONNECT`
    /// (or, below API 31, location). Distinct from `NotFound` on purpose — a
    /// denied permission produces an empty scan that is indistinguishable from
    /// a device that is switched off, and telling a user to check their device
    /// when the real problem is a permission dialog they dismissed is the
    /// worst answer this code could give.
    PermissionDenied,
    /// The radio is switched off at the OS level.
    AdapterOff,
}

impl std::fmt::Display for BleError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            BleError::Ble(e) => write!(f, "bluetooth: {e}"),
            BleError::Wire(e) => write!(f, "{e}"),
            BleError::NotFound { scanned_for } => write!(
                f,
                "no device found.\n\
                 scanned {}s for advertisements of service {SERVICE_UUID}\n\
                 check: the device is powered on, BLE (not USB) is the selected\n\
                 transport — see PROTOCOL.md 3b, it advertises on only one — and\n\
                 that `bluetoothctl scan on` sees it at all",
                scanned_for.as_secs()
            ),
            BleError::NotAdvertising { scanned_for } => write!(
                f,
                "{NOT_ADVERTISING} (Scanned {}s, then tried the remembered id directly.)",
                scanned_for.as_secs()
            ),
            BleError::NoAdapter => write!(
                f,
                "no Bluetooth adapter. `hciconfig` should list hci0 as UP RUNNING"
            ),
            BleError::MissingCharacteristic(u) => write!(
                f,
                "connected, but the peer has no characteristic {u} — wrong device, \
                 or firmware older than the fixed UUIDs"
            ),
            BleError::NotNotifiable(u) => {
                write!(f, "characteristic {u} does not support notify")
            }
            BleError::Timeout => write!(f, "timed out waiting for the device to answer"),
            BleError::PermissionDenied => write!(
                f,
                "Bluetooth permission not granted.\n\
                 Android needs \"Nearby devices\" (BLUETOOTH_SCAN and BLUETOOTH_CONNECT)\n\
                 to see a wallet advertising; without it every scan comes back empty\n\
                 even with the device sitting next to the phone. If no dialog appeared,\n\
                 grant it under Settings → Apps → LeekWallet → Permissions → Nearby devices."
            ),
            BleError::AdapterOff => write!(
                f,
                "Bluetooth is switched off. Turn it on and scan again."
            ),
        }
    }
}

impl std::error::Error for BleError {}

impl From<WireError> for BleError {
    fn from(e: WireError) -> Self {
        BleError::Wire(e)
    }
}

#[cfg(feature = "ble")]
impl From<btleplug::Error> for BleError {
    fn from(e: btleplug::Error) -> Self {
        BleError::Ble(e.to_string())
    }
}

#[cfg(feature = "blec")]
impl From<tauri_plugin_blec::Error> for BleError {
    fn from(e: tauri_plugin_blec::Error) -> Self {
        BleError::Ble(e.to_string())
    }
}

/// One advertising peer, as far as a chooser needs to know it.
///
/// `id` is the platform's own handle (a MAC on BlueZ and Android, a UUID on
/// CoreBluetooth) and is what `connect_id` matches on. `name` is decoration
/// only — it is host-supplied, user-settable under T56, and must never be the
/// thing an identity decision rests on.
#[derive(Debug, Clone)]
pub struct BleDevice {
    pub id: String,
    pub name: Option<String>,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn not_advertising_says_what_to_do() {
        let text = BleError::NotAdvertising { scanned_for: Duration::from_secs(4) }.to_string();
        assert!(text.starts_with("The board is not advertising"));
        assert!(text.contains("connected to this or another app"));
        assert!(text.contains("Restart the board or wait ~30 s"));
        assert!(text.contains("button"));
        assert!(text.contains("4s"));
    }
}
