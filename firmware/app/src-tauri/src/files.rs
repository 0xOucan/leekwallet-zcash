//! Saving a file the board produced: the `.zsig` of a signed PCZT.
//!
//! A webview cannot save on its own here. An anchor with `download` does
//! nothing in the desktop webviews or the Android one, so the bytes come
//! to this side and are written to the downloads folder: `~/Downloads` on
//! desktop, the app's own `Download` folder on Android (Tauri resolves
//! that to the app-specific external files directory, which needs no
//! storage permission and is reachable over USB file transfer).
//!
//! The name is the app's own (`zsig_name` in packages/core/src/zcash.ts),
//! still checked here: one path component, a fixed extension, and never an
//! overwrite, so a second save of `send.zsig` becomes `send-1.zsig`.

use std::path::{Path, PathBuf};

use tauri::Manager;

/// The extensions this command will write. Anything else is refused: the
/// command exists for the board's output, not as a general file writer.
const ALLOWED: &[&str] = &["zsig"];

/// Checks `name` and picks a path in `dir` that does not exist yet.
pub fn target_path(dir: &Path, name: &str) -> Result<PathBuf, String> {
    let name = name.trim();
    if name.is_empty() || name.len() > 120 || name.contains(['/', '\\', '\0']) || name.starts_with('.') {
        return Err(format!("{name:?} is not a file name this app writes"));
    }
    let (stem, ext) = name.rsplit_once('.').ok_or("the file name has no extension")?;
    if !ALLOWED.contains(&ext) || stem.is_empty() {
        return Err(format!("this app only saves {} files", ALLOWED.join(", ")));
    }
    let mut path = dir.join(name);
    let mut n = 1;
    while path.exists() {
        path = dir.join(format!("{stem}-{n}.{ext}"));
        n += 1;
    }
    Ok(path)
}

/// Writes `bytes` to the downloads folder under `name`; returns the path.
#[tauri::command]
pub fn save_download(app: tauri::AppHandle, name: String, bytes: Vec<u8>) -> Result<String, String> {
    let dir = app.path().download_dir().map_err(|e| format!("no downloads folder: {e}"))?;
    std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let path = target_path(&dir, &name)?;
    std::fs::write(&path, &bytes).map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(path.display().to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_are_one_component_with_a_known_extension() {
        let dir = std::env::temp_dir();
        for bad in ["", "../x.zsig", "a/b.zsig", ".zsig", "x.exe", "x", "x.pczt"] {
            assert!(target_path(&dir, bad).is_err(), "{bad:?}");
        }
        assert!(target_path(&dir, "send.zsig").is_ok());
    }

    #[test]
    fn an_existing_file_is_never_overwritten() {
        let dir = std::env::temp_dir().join(format!("leek-files-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("send.zsig"), b"x").unwrap();
        std::fs::write(dir.join("send-1.zsig"), b"x").unwrap();
        assert_eq!(target_path(&dir, "send.zsig").unwrap(), dir.join("send-2.zsig"));
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
