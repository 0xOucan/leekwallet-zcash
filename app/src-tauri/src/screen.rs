//! The phone's screen while the Sign-with-QR code is up (EXPERIMENTAL):
//! full window brightness and the screen kept on, both put back after.
//! The display rules are `leek_ur::display` (the SDK's `qr_display_hints()`):
//! from the bench of 2026-11-11, a phone at half brightness left the board's
//! camera at its longest exposure with a noisy picture, and a screen that
//! dims or locks mid-loop starts the board over.
//!
//! Android only; the desktop answers `false` and changes nothing. The same
//! way in as `allow_camera` (lib.rs) on Linux: the platform webview, reached
//! from Rust through `with_webview`, here its JNI handle, which runs on the
//! activity's UI thread. Nothing is added to the generated Android project
//! (gen/ is regenerated) and no permission is needed: a window's own
//! brightness and `FLAG_KEEP_SCREEN_ON` are the app's to set. What no app can
//! set is the system's anti-flicker (DC dimming) on OLED phones, so the
//! webview shows the SDK's tip for it under the code.

/// `on`: full brightness and keep the screen on; off: the system brightness
/// and the usual timeout again. True when this build changed anything.
#[tauri::command]
pub fn qr_screen(window: tauri::WebviewWindow, on: bool) -> Result<bool, String> {
    #[cfg(target_os = "android")]
    {
        android::set(&window, on)?;
        Ok(true)
    }
    #[cfg(not(target_os = "android"))]
    {
        let _ = (window, on);
        Ok(false)
    }
}

#[cfg(target_os = "android")]
mod android {
    use jni::objects::{JObject, JValue};
    use jni::JNIEnv;

    /// `WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON`.
    const FLAG_KEEP_SCREEN_ON: i32 = 0x0000_0080;
    /// `screenBrightness`: 1.0 is full, -1 (`BRIGHTNESS_OVERRIDE_NONE`)
    /// follows the system again.
    const BRIGHTNESS_FULL: f32 = 1.0;
    const BRIGHTNESS_OVERRIDE_NONE: f32 = -1.0;

    pub fn set(window: &tauri::WebviewWindow, on: bool) -> Result<(), String> {
        window
            .with_webview(move |w| {
                w.jni_handle().exec(move |env, activity, _webview| {
                    if let Err(e) = apply(env, activity, on) {
                        // A Java exception left pending would fail the next JNI call.
                        let _ = env.exception_clear();
                        eprintln!("qr screen: could not set the window ({e})");
                    }
                });
            })
            .map_err(|e| e.to_string())
    }

    fn apply(env: &mut JNIEnv, activity: &JObject, on: bool) -> jni::errors::Result<()> {
        let win = env.call_method(activity, "getWindow", "()Landroid/view/Window;", &[])?.l()?;
        let flags = if on { "addFlags" } else { "clearFlags" };
        env.call_method(&win, flags, "(I)V", &[JValue::Int(FLAG_KEEP_SCREEN_ON)])?;
        let lp = env
            .call_method(&win, "getAttributes", "()Landroid/view/WindowManager$LayoutParams;", &[])?
            .l()?;
        let level = if on { BRIGHTNESS_FULL } else { BRIGHTNESS_OVERRIDE_NONE };
        env.set_field(&lp, "screenBrightness", "F", JValue::Float(level))?;
        env.call_method(&win, "setAttributes", "(Landroid/view/WindowManager$LayoutParams;)V", &[JValue::Object(&lp)])?;
        Ok(())
    }
}
