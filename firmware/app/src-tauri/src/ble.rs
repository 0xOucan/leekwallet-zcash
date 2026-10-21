//! BLE GATT transport commands. Desktop and Android, from one source.
//!
//! Nothing here is platform-conditional: `leek-transport-ble` picks its radio
//! backend at build time and hands back the same `BleTransport` either way.
//! That is on purpose — a phone-specific command surface would be a second
//! implementation of the signing path, and the one that gets less use is the
//! one that quietly rots.
//!
//! A deliberate mirror of `serial.rs`: same four commands, same shapes, same
//! request/response model. The frontend's two transports differ only in which
//! command names they invoke, which is what lets the client above them stay
//! transport-blind.
//!
//! The one structural difference is `tokio::sync::Mutex` rather than `std`'s.
//! btleplug is async all the way down, so the guard is held across `.await`,
//! and a `std` guard held across an await point is not `Send`.

use std::time::Duration;

use leek_transport_ble::{BleDevice, BleTransport};
use serde::Serialize;
use tauri::{AppHandle, Manager};
use tokio::sync::Mutex;

/// How long a scan looks before giving up.
///
/// Long enough for a device that is advertising to be seen, short enough that
/// a user who forgot to switch the device off USB is told so rather than left
/// watching a spinner. `scan()` always waits the whole window; `connect_id()`
/// returns as soon as it matches.
const SCAN_MS: u64 = 4000;

#[derive(Serialize)]
pub struct Device {
    id: String,
    /// Advertised name, if any. Decoration — the service UUID is what actually
    /// identified this peer, and the name is host-supplied and user-settable.
    name: Option<String>,
}

impl From<BleDevice> for Device {
    fn from(d: BleDevice) -> Self {
        Device { id: d.id, name: d.name }
    }
}

/// One connection at a time, for the same reason as serial: the device is a
/// single physical object and the frame stream has no request IDs.
#[derive(Default)]
pub struct Connection(Mutex<Option<BleTransport>>);

#[tauri::command]
pub async fn ble_scan() -> Result<Vec<Device>, String> {
    guarded("ble_scan", async {
        BleTransport::scan(Duration::from_millis(SCAN_MS))
            .await
            .map(|v| v.into_iter().map(Device::from).collect())
            .map_err(|e| e.to_string())
    })
    .await
}

#[tauri::command]
pub async fn ble_connect(id: String, app: AppHandle) -> Result<(), String> {
    guarded("ble_connect", async move {
        let state = app.state::<Connection>();
        // Tear the previous link down FIRST. Assigning over it would only drop the
        // transport, and neither backend implements `Drop`, so the CCCD would stay
        // written and `disconnect()` would never run — which is the bug already
        // fixed inside `BleTransport::disconnect`, reachable again by the implicit
        // path. BlueZ then still believes the client is subscribed, the next
        // subscribe is a no-op, and the device answers correctly into a dead
        // channel. Reachable in normal use: a drop the frontend never noticed,
        // followed by the user pressing Connect again.
        //
        // The old link's failure is not this call's failure. It is gone either way,
        // and refusing a new connection because a corpse would not close cleanly is
        // the wrong trade.
        if let Some(mut old) = state.0.lock().await.take() {
            let _ = old.disconnect().await;
        }

        // `connect_id` also closes a link the radio stack still holds with no
        // transport here (Android), scans, and tries the id directly when the
        // board did not advertise; a board found neither way is reported as
        // "not advertising", with what to do (BleError::NotAdvertising).
        let transport = BleTransport::connect_id(&id, Duration::from_millis(SCAN_MS))
            .await
            .map_err(|e| e.to_string())?;
        *state.0.lock().await = Some(transport);
        Ok(())
    })
    .await
}

/// Drop the link.
///
/// Explicitly disconnecting rather than just dropping the transport: the
/// device serves one peer at a time (PROTOCOL.md 3b), so a link left for the
/// stack to time out keeps the next connection out for as long as that takes.
#[tauri::command]
pub async fn ble_disconnect(app: AppHandle) -> Result<(), String> {
    guarded("ble_disconnect", async move {
        let taken = app.state::<Connection>().0.lock().await.take();
        if let Some(mut transport) = taken {
            transport.disconnect().await.map_err(|e| e.to_string())?;
        }
        Ok(())
    })
    .await
}

/// Send one frame and wait for the reply.
///
/// Bytes cross the bridge as plain arrays, matching `serial::request` — see
/// the note there.
#[tauri::command]
pub async fn ble_request(
    frame_type: u8,
    payload: Vec<u8>,
    timeout_ms: u64,
    app: AppHandle,
) -> Result<(u8, Vec<u8>), String> {
    guarded("ble_request", async move {
        let state = app.state::<Connection>();
        let mut guard = state.0.lock().await;
        let transport = guard.as_mut().ok_or("not connected")?;

        transport.send(frame_type, &payload).await.map_err(|e| e.to_string())?;
        transport
            .recv(Duration::from_millis(timeout_ms))
            .await
            .map_err(|e| e.to_string())
    })
    .await
}

/// Close the Bluetooth link, whoever holds it: the transport of this app's
/// session, and on Android a link the radio stack still holds without one.
/// Bounded; true when there was something to close. For the lifecycle
/// (link.rs): the app going to the background, closing, or starting in a
/// process that still holds the previous run's link.
pub async fn release(app: &AppHandle) -> bool {
    let taken = match tokio::time::timeout(RELEASE_WAIT, app.state::<Connection>().0.lock()).await {
        Ok(mut guard) => guard.take(),
        // A request is waiting on the board (a review on its screen): the
        // session is in use, so it is not released behind the user's back.
        Err(_) => return false,
    };
    let had = taken.is_some() || BleTransport::link_open();
    if let Some(mut transport) = taken {
        let _ = tokio::time::timeout(RELEASE_WAIT, transport.disconnect()).await;
    }
    BleTransport::release_link().await;
    had
}

/// How long releasing the link may take before the app gives up on it.
const RELEASE_WAIT: Duration = Duration::from_secs(3);

/// Run a command's future on its own task, so a panic in it (blec asserts
/// on states it does not expect) fails that call with a message instead of
/// leaving the webview's promise unsettled. Logged for `adb logcat -s
/// RustStdoutStderr`.
async fn guarded<T, F>(what: &'static str, f: F) -> Result<T, String>
where
    F: std::future::Future<Output = Result<T, String>> + Send + 'static,
    T: Send + 'static,
{
    match tauri::async_runtime::spawn(f).await {
        Ok(Ok(v)) => Ok(v),
        Ok(Err(e)) => {
            eprintln!("leek-ble: {what} failed: {e}");
            Err(e)
        }
        Err(e) => {
            eprintln!("leek-ble: {what} stopped unexpectedly: {e}");
            Err(format!(
                "Bluetooth: the {what} worker stopped unexpectedly ({e}). For a bug report: \
                 adb logcat -s RustStdoutStderr Peripheral AndroidRuntime"
            ))
        }
    }
}
