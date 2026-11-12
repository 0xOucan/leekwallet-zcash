//! The phone's screen while a QR code for the board is up (Sign with QR,
//! Check address; EXPERIMENTAL): the screen kept on, and the window at the
//! brightness the user picked on the QR page (the system's by default), both
//! put back after. The display rules are `leek_ur::display` (the SDK's
//! `qr_display_hints()`): forcing 100 % washed the code out for the board on
//! an OLED phone (2026-11-11), so brightness is the user's, in steps; a
//! screen that dims or locks mid-loop starts the board over.
//!
//! Android only; the desktop answers `false` and changes nothing. The same
//! way in as `allow_camera` (lib.rs) on Linux: the platform webview, reached
//! from Rust through `with_webview`, here its JNI handle, which runs on the
//! activity's UI thread. Nothing is added to the generated Android project
//! (gen/ is regenerated) and no permission is needed: a window's own
//! brightness and `FLAG_KEEP_SCREEN_ON` are the app's to set. What no app can
//! set is the system's anti-flicker (DC dimming) on OLED phones, so the
//! webview shows the SDK's tip for it under the code.

/// The window brightness for `level` (0.0–1.0; `None` is the system's):
/// what Android's `screenBrightness` takes, -1 being
/// `BRIGHTNESS_OVERRIDE_NONE`. Out-of-range or NaN levels follow the system.
pub fn window_brightness(on: bool, level: Option<f32>) -> f32 {
    match level {
        Some(l) if on && l.is_finite() && l > 0.0 => l.clamp(0.01, 1.0),
        _ => -1.0,
    }
}

/// `on`: keep the screen on, at `level` (`None`: the system brightness);
/// off: the system brightness and the usual timeout again. True when this
/// build changed anything.
#[tauri::command]
pub fn qr_screen(window: tauri::WebviewWindow, on: bool, level: Option<f32>) -> Result<bool, String> {
    let brightness = window_brightness(on, level);
    #[cfg(target_os = "android")]
    {
        android::set(&window, on, brightness)?;
        Ok(true)
    }
    #[cfg(not(target_os = "android"))]
    {
        let _ = (window, brightness);
        Ok(false)
    }
}

#[cfg(test)]
mod tests {
    use super::window_brightness;

    #[test]
    fn the_level_is_the_users_and_system_by_default() {
        assert_eq!(window_brightness(true, None), -1.0);
        assert_eq!(window_brightness(true, Some(0.6)), 0.6);
        assert_eq!(window_brightness(true, Some(1.0)), 1.0);
        assert_eq!(window_brightness(true, Some(7.0)), 1.0);
        assert_eq!(window_brightness(true, Some(f32::NAN)), -1.0);
        assert_eq!(window_brightness(true, Some(0.0)), -1.0);
        // Off: always the system's again.
        assert_eq!(window_brightness(false, Some(0.8)), -1.0);
    }
}

#[cfg(target_os = "android")]
mod android {
    use jni::objects::{JObject, JValue};
    use jni::JNIEnv;

    /// `WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON`.
    const FLAG_KEEP_SCREEN_ON: i32 = 0x0000_0080;

    /// `brightness`: `screenBrightness`, 0.01–1.0, or -1
    /// (`BRIGHTNESS_OVERRIDE_NONE`) to follow the system.
    pub fn set(window: &tauri::WebviewWindow, on: bool, brightness: f32) -> Result<(), String> {
        window
            .with_webview(move |w| {
                w.jni_handle().exec(move |env, activity, _webview| {
                    if let Err(e) = apply(env, activity, on, brightness) {
                        // A Java exception left pending would fail the next JNI call.
                        let _ = env.exception_clear();
                        eprintln!("qr screen: could not set the window ({e})");
                    }
                });
            })
            .map_err(|e| e.to_string())
    }

    fn apply(env: &mut JNIEnv, activity: &JObject, on: bool, brightness: f32) -> jni::errors::Result<()> {
        let win = env.call_method(activity, "getWindow", "()Landroid/view/Window;", &[])?.l()?;
        let flags = if on { "addFlags" } else { "clearFlags" };
        env.call_method(&win, flags, "(I)V", &[JValue::Int(FLAG_KEEP_SCREEN_ON)])?;
        let lp = env
            .call_method(&win, "getAttributes", "()Landroid/view/WindowManager$LayoutParams;", &[])?
            .l()?;
        env.set_field(&lp, "screenBrightness", "F", JValue::Float(brightness))?;
        env.call_method(&win, "setAttributes", "(Landroid/view/WindowManager$LayoutParams;)V", &[JValue::Object(&lp)])?;
        Ok(())
    }
}
