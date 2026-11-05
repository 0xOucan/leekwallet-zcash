//! The compatibility levels and texts are repeated where Rust cannot reach:
//! the firmware, the app's TypeScript core and the Chrome extension. This keeps
//! them equal, and pins what a Kotlin app relies on.

use leek_device::device::{ERR_APP_OUTDATED, HOST_COMPAT};
use leek_zcash::{Features, LeekError, BOARD_UPDATE_TEXT, MIN_FIRMWARE_COMPAT};

fn read(path: &str) -> String {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    std::fs::read_to_string(root.join(path)).unwrap()
}

#[test]
fn every_copy_says_the_same() {
    let fw = read("firmware/src/version.h");
    assert!(fw.contains(&format!("#define LEEK_FW_COMPAT       {MIN_FIRMWARE_COMPAT}\n")));
    assert!(fw.contains(&format!("#define LEEK_HOST_COMPAT_MIN {HOST_COMPAT}\n")));
    assert_eq!(ERR_APP_OUTDATED, 0x0519);

    let app_update = LeekError::AppOutdated.to_string();
    for host in ["app/packages/core/src/zcash.ts", "chrome/extension/src/board/device.js"] {
        let s = read(host);
        assert!(s.contains(&format!("HOST_COMPAT = {HOST_COMPAT};")), "{host}");
        assert!(s.contains(&format!("MIN_FIRMWARE_COMPAT = {MIN_FIRMWARE_COMPAT};")), "{host}");
        assert!(s.contains(BOARD_UPDATE_TEXT), "{host}");
        assert!(app_update.contains(update_sentence(&s)), "{host}: {app_update}");
    }
}

/// The sentence after `APP_UPDATE_TEXT =`, whichever quotes the file uses.
fn update_sentence(s: &str) -> &str {
    let after = &s[s.find("APP_UPDATE_TEXT =").unwrap()..];
    let start = after.find("Update your app").unwrap();
    let rest = &after[start..];
    &rest[..rest.find(['"', '\'']).unwrap()]
}

#[test]
fn a_board_that_reports_no_level_is_outdated() {
    // What a v0.1.0 board answers: zGetFeatures with no fwCompat.
    let v010 = Features { fw_compat: None, ..Features::default() };
    assert!(v010.firmware_outdated());
    assert!(Features { fw_compat: Some(1), ..Features::default() }.firmware_outdated());
    assert!(!Features { fw_compat: Some(MIN_FIRMWARE_COMPAT), ..Features::default() }.firmware_outdated());
    assert!(!Features { fw_compat: Some(MIN_FIRMWARE_COMPAT + 1), ..Features::default() }.firmware_outdated());
}
