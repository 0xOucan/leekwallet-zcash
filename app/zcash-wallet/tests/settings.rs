//! The saved settings: the lightwalletd server per network and the
//! optional 1Click API key. Offline, in a temporary directory standing in
//! for the wallets' root.

use std::path::PathBuf;

use leek_zcash_wallet::settings;
use zecz::Network;

const MAIN: Network = Network::MainNetwork;
const TEST: Network = Network::TestNetwork;

struct TempDir(PathBuf);

impl TempDir {
    fn new(tag: &str) -> Self {
        let p = std::env::temp_dir().join(format!("leekz-settings-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&p);
        std::fs::create_dir_all(&p).unwrap();
        TempDir(p)
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn err_text<T>(r: anyhow::Result<T>) -> String {
    match r {
        Ok(_) => panic!("expected an error"),
        Err(e) => format!("{e:#}"),
    }
}

#[test]
fn the_1click_api_key_is_kept_and_never_written_when_absent() {
    let d = TempDir::new("apikey");
    let path = settings::path_in(&d.0);
    // A file without the key is written without it.
    settings::save_server(&path, MAIN, Some("https://zec.example.net:443")).unwrap();
    assert!(!std::fs::read_to_string(&path).unwrap().contains("oneClickApiKey"));
    // A key set by hand survives a server change.
    let mut s = settings::load(&path).unwrap();
    s.one_click_api_key = Some("partner.jwt".into());
    settings::save(&path, &s).unwrap();
    settings::save_server(&path, TEST, Some("https://testnet.example:443")).unwrap();
    assert_eq!(settings::load(&path).unwrap().one_click_api_key.as_deref(), Some("partner.jwt"));
    assert!(std::fs::read_to_string(&path).unwrap().contains("\"oneClickApiKey\": \"partner.jwt\""));
}

#[test]
fn servers_are_kept_per_network() {
    let d = TempDir::new("servers");
    let path = settings::path_in(&d.0);

    let s = settings::load(&path).unwrap();
    assert_eq!(s, settings::Settings::default());
    assert!(s.is_default_server(MAIN) && s.is_default_server(TEST));

    let s = settings::save_server(&path, MAIN, Some(" https://zec.example.net:443 ")).unwrap();
    assert_eq!(s.server_for(MAIN), "https://zec.example.net:443");
    assert_eq!(s.server_for(TEST), "https://testnet.zec.rocks:443", "testnet keeps its default");
    assert!(!s.is_default_server(MAIN) && s.is_default_server(TEST));
    assert_eq!(settings::load(&path).unwrap().server_for(MAIN), "https://zec.example.net:443");

    // Empty, None or the default URL go back to the default.
    for back in [None, Some(""), Some("https://zec.rocks:443")] {
        settings::save_server(&path, MAIN, Some("http://127.0.0.1:9067")).unwrap();
        let s = settings::save_server(&path, MAIN, back).unwrap();
        assert!(s.is_default_server(MAIN), "{back:?}");
        assert_eq!(s.server_for(MAIN), "https://zec.rocks:443");
    }

    // Not a URL: refused, nothing saved.
    let before = std::fs::read(&path).unwrap();
    for bad in ["zec.rocks:443", "ftp://zec.rocks", "https://", "https://zec rocks", "https:///x"] {
        let e = err_text(settings::save_server(&path, MAIN, Some(bad)));
        assert!(e.contains("https://host:port"), "{bad}: {e}");
    }
    assert_eq!(std::fs::read(&path).unwrap(), before);
}

#[test]
fn a_bad_settings_file_is_reported_and_replaced_by_a_save() {
    let d = TempDir::new("bad");
    let path = settings::path_in(&d.0);
    std::fs::write(&path, b"{\"version\": 1, \"network\": \"regtest\"}").unwrap();
    assert!(err_text(settings::load(&path)).contains("unknown network"));
    std::fs::write(&path, b"{\"version\": 9, \"network\": \"main\"}").unwrap();
    assert!(err_text(settings::load(&path)).contains("settings format 9"));
    std::fs::write(&path, b"not json").unwrap();
    assert!(err_text(settings::load(&path)).contains("not a settings file"));
    // Preferences only: saving a server writes a good file over it.
    settings::save_server(&path, TEST, Some("https://testnet.example:443")).unwrap();
    assert_eq!(settings::load(&path).unwrap().server_for(TEST), "https://testnet.example:443");
    assert!(settings::parse_network("mainnet").is_ok() && settings::parse_network("Main").is_err());
}
