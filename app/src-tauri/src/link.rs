//! The board link across the app's lifecycle: released when the app goes
//! to the background for a while, when it closes, and when a new webview
//! starts in a process that still holds the previous run's link.
//!
//! Why: the board serves one peer at a time and does not advertise while a
//! peer is connected (firmware src/ble.c). On Android, closing the app with
//! Back destroys the activity but often not the process, and the Bluetooth
//! link the process held stayed open with nobody using it: the board kept
//! it, never advertised, and Reconnect failed until the board was
//! power-cycled (the tablet test of 2026-10-20). A link this app holds and
//! does not use is now closed with a GATT disconnect, so the board starts
//! advertising again at once.
//!
//! Not on every pause: Android pauses the app for its own permission
//! dialogs (USB, nearby devices, the camera) and while the user copies an
//! address elsewhere, and a session released there would need a new
//! passkey and ALLOW on the board. So the release waits `BACKGROUND_GRACE`
//! in the background first, and never interrupts a request waiting on the
//! board (a review on its screen holds the link's lock).

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::Duration;

use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager};

/// How long the app may stay in the background before its link is closed.
pub const BACKGROUND_GRACE: Duration = Duration::from_secs(30);

/// The event the webview hears when a link was closed under it.
pub const RELEASED_EVENT: &str = "link://released";

/// Whether a background release that was scheduled may still run: each
/// pause and resume moves the generation, so only the latest pause's timer
/// acts, and only while the app is still in the background.
#[derive(Default)]
pub struct Lifecycle {
    generation: AtomicU64,
    background: AtomicBool,
}

impl Lifecycle {
    /// The app went to the background: the generation its timer carries.
    pub fn suspend(&self) -> u64 {
        self.background.store(true, Ordering::SeqCst);
        self.generation.fetch_add(1, Ordering::SeqCst) + 1
    }

    /// Back in front: every pending release is void.
    pub fn resume(&self) {
        self.background.store(false, Ordering::SeqCst);
        self.generation.fetch_add(1, Ordering::SeqCst);
    }

    /// True when the pause of `generation` is still the current state.
    pub fn still_suspended(&self, generation: u64) -> bool {
        self.background.load(Ordering::SeqCst) && self.generation.load(Ordering::SeqCst) == generation
    }
}

#[derive(Clone, Serialize)]
struct Released {
    reason: &'static str,
}

/// Close the Bluetooth and USB links; true when one was open. Bounded.
pub async fn release_all(app: &AppHandle, reason: &'static str) -> bool {
    let ble = crate::ble::release(app).await;
    let usb = crate::serial::release(app).await;
    let any = ble || usb;
    if any {
        eprintln!("leek-link: released the board link ({reason})");
        let _ = app.emit(RELEASED_EVENT, Released { reason });
    }
    any
}

/// The app went to the background (Android onPause).
#[cfg(mobile)]
pub fn suspended(app: &AppHandle) {
    let generation = app.state::<Lifecycle>().suspend();
    let app = app.clone();
    tauri::async_runtime::spawn(async move {
        tokio::time::sleep(BACKGROUND_GRACE).await;
        if app.state::<Lifecycle>().still_suspended(generation) {
            release_all(&app, "background").await;
        }
    });
}

/// Back in front (Android onResume).
#[cfg(mobile)]
pub fn resumed(app: &AppHandle) {
    app.state::<Lifecycle>().resume();
}

/// The app is closing: release now, waiting a few seconds at most.
pub fn closing(app: &AppHandle) {
    let app = app.clone();
    let _ = tauri::async_runtime::block_on(async move {
        tokio::time::timeout(Duration::from_secs(4), release_all(&app, "closing")).await
    });
}

/// A webview starts with no session, so any link this process holds is
/// left from a previous run: close it, so the board advertises again.
/// True when there was one.
#[tauri::command]
pub async fn link_reset(app: AppHandle) -> Result<bool, String> {
    let task = tauri::async_runtime::spawn(async move { release_all(&app, "a new start").await });
    task.await.map_err(|e| format!("closing the previous link failed: {e}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_pause_releases_only_if_nothing_moved() {
        let l = Lifecycle::default();
        let g = l.suspend();
        assert!(l.still_suspended(g));
        l.resume();
        assert!(!l.still_suspended(g), "back in front voids the release");
    }

    #[test]
    fn only_the_latest_pause_counts() {
        let l = Lifecycle::default();
        let first = l.suspend();
        l.resume();
        let second = l.suspend();
        assert!(!l.still_suspended(first), "an older pause's timer does nothing");
        assert!(l.still_suspended(second));
    }

    #[test]
    fn in_front_from_the_start() {
        let l = Lifecycle::default();
        assert!(!l.still_suspended(0));
    }

    #[test]
    fn the_grace_outlasts_a_permission_dialog() {
        // Android pauses the app for its own dialogs; 30 s is longer than
        // anyone takes to answer one, shorter than a phone left on a desk.
        assert!(BACKGROUND_GRACE >= Duration::from_secs(20));
        assert!(BACKGROUND_GRACE <= Duration::from_secs(60));
    }
}
