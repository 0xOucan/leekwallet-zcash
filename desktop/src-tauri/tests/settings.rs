//! The network switch (roadmap 10.3): what `set_network` and `set_server`
//! save, and the profile each network opens. Offline, in a temporary
//! directory standing in for the app data dir.

use std::path::PathBuf;

use leekwallet_zcash_lib::{backend, contacts, settings};
use zecz::wallet::{AccountBirthday, AccountSpec};
use zecz::{Network, Wallet};

const MAIN: Network = Network::MainNetwork;
const TEST: Network = Network::TestNetwork;

struct TempDir(PathBuf);

impl TempDir {
    fn new(tag: &str) -> Self {
        let p = std::env::temp_dir().join(format!("leekz-desktop-settings-{tag}-{}", std::process::id()));
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
fn testnet_until_switched_and_the_switch_is_saved() {
    let d = TempDir::new("switch");
    let app = &d.0;
    let path = settings::path_in(app);

    // First run: no file, testnet with its default server.
    let s = settings::load(&path).unwrap();
    assert_eq!(s, settings::Settings::default());
    let p = s.profile(app).unwrap();
    assert_eq!((p.net_name(), p.server.as_str()), ("test", "https://testnet.zec.rocks:443"));
    assert_eq!(p.data_dir, app.join("wallets/test"));
    assert!(!path.exists(), "loading writes nothing");

    // Switch to mainnet: saved, and the mainnet wallet's own directory.
    let (s, p) = settings::switch_network(&path, app, MAIN).unwrap();
    assert_eq!(s.network, "main");
    assert_eq!((p.net_name(), p.server.as_str(), p.coin_type()), ("main", "https://zec.rocks:443", 133));
    assert_eq!(p.data_dir, app.join("wallets/main"));
    let again = settings::load(&path).unwrap();
    assert_eq!(again.profile(app).unwrap().data_dir, app.join("wallets/main"), "the choice survives a restart");
    assert!(!app.join("settings.json.tmp").exists());
    let json: serde_json::Value = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
    assert_eq!(json, serde_json::json!({"version": 1, "network": "main", "servers": {}}));

    // And back.
    let (_, p) = settings::switch_network(&path, app, TEST).unwrap();
    assert_eq!(p.data_dir, app.join("wallets/test"));
    assert_eq!(settings::load(&path).unwrap().network, "test");
}

#[test]
fn each_network_keeps_its_own_wallet_and_contacts() {
    let d = TempDir::new("separate");
    let app = &d.0;
    let path = settings::path_in(app);

    // A testnet wallet with an account, then a switch to mainnet.
    let kv = |name: &str| -> serde_json::Value {
        let p = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../vectors/keys.json");
        let all: Vec<serde_json::Value> = serde_json::from_slice(&std::fs::read(p).unwrap()).unwrap();
        all.into_iter().find(|v| v["name"] == name).unwrap()
    };
    let birthday = |h: u32| {
        use zcash_client_backend::data_api::chain::ChainState;
        use zcash_primitives::block::BlockHash;
        use zcash_protocol::consensus::BlockHeight;
        AccountBirthday::from_parts(ChainState::empty(BlockHeight::from_u32(h), BlockHash([0; 32])), None)
    };
    let tp = settings::Settings::default().profile(app).unwrap();
    let t = kv("test/abandon-art/pp0/acct0");
    Wallet::init_with_birthday(tp.clone(), &AccountSpec::view_only(t["ufvk"].as_str().unwrap().into(), "w"), &birthday(3_000_000)).unwrap();
    contacts::save(&contacts::path_in(app, TEST), TEST, "Faucet", t["ua"].as_str().unwrap(), None).unwrap();

    let (_, mp) = settings::switch_network(&path, app, MAIN).unwrap();
    let st = backend::profile_status(&mp).unwrap();
    assert_eq!(st.network, "main");
    assert!(!st.has_account, "the mainnet wallet starts empty");
    assert!(contacts::list(&contacts::path_in(app, MAIN)).unwrap().is_empty(), "testnet contacts leaked");
    let m = kv("main/abandon-art/pp0/acct0");
    Wallet::init_with_birthday(mp.clone(), &AccountSpec::view_only(m["ufvk"].as_str().unwrap().into(), "w"), &birthday(3_500_000)).unwrap();
    assert_eq!(backend::addresses(&mp).unwrap().unified, m["ua"].as_str().unwrap());

    // Back on testnet, the testnet wallet and contacts are as they were.
    let (_, tp2) = settings::switch_network(&path, app, TEST).unwrap();
    assert_eq!(backend::addresses(&tp2).unwrap().unified, t["ua"].as_str().unwrap());
    assert_eq!(contacts::list(&contacts::path_in(app, TEST)).unwrap().len(), 1);
    // A wallet directory never opens as the other network.
    assert!(err_text(Wallet::open(zecz::Profile::new(MAIN, app.join("wallets/test"), None))).contains("holds a test wallet"));
}

#[test]
fn servers_are_kept_per_network() {
    let d = TempDir::new("servers");
    let app = &d.0;
    let path = settings::path_in(app);

    let s = settings::save_server(&path, MAIN, Some(" https://zec.example.net:443 ")).unwrap();
    assert_eq!(s.server_for(MAIN), "https://zec.example.net:443");
    assert_eq!(s.server_for(TEST), "https://testnet.zec.rocks:443", "testnet keeps its default");
    assert!(!s.is_default_server(MAIN) && s.is_default_server(TEST));

    // The server follows the network through a switch and a restart.
    let (_, p) = settings::switch_network(&path, app, MAIN).unwrap();
    assert_eq!(p.server, "https://zec.example.net:443");
    let (_, p) = settings::switch_network(&path, app, TEST).unwrap();
    assert_eq!(p.server, "https://testnet.zec.rocks:443");
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
fn the_dto_names_the_network_and_its_server() {
    let d = TempDir::new("dto");
    let app = &d.0;
    let s = settings::Settings::default();
    let dto = settings::NetworkDto::of(&s.profile(app).unwrap(), &s, false);
    assert_eq!(dto.network, "test");
    assert_eq!(dto.server, dto.default_server);
    assert!(!dto.custom_server && !dto.from_environment);

    let mut s = s;
    s.network = "main".into();
    s.set_server(MAIN, Some("https://zec.example.net:443")).unwrap();
    let p = s.profile(app).unwrap();
    let dto = settings::NetworkDto::of(&p, &s, true);
    assert_eq!((dto.network.as_str(), dto.server.as_str()), ("main", "https://zec.example.net:443"));
    assert_eq!(dto.default_server, "https://zec.rocks:443");
    assert!(dto.custom_server);
    // The environment matters only when it differs from what is saved.
    assert!(!dto.from_environment);
    let env = zecz::Profile::new(TEST, app.join("wallets/test"), None);
    assert!(settings::NetworkDto::of(&env, &s, true).from_environment);
}

#[test]
fn a_bad_settings_file_is_reported_and_replaced_by_a_switch() {
    let d = TempDir::new("bad");
    let app = &d.0;
    let path = settings::path_in(app);
    std::fs::write(&path, b"{\"version\": 1, \"network\": \"regtest\"}").unwrap();
    assert!(err_text(settings::load(&path)).contains("unknown network"));
    std::fs::write(&path, b"{\"version\": 9, \"network\": \"main\"}").unwrap();
    assert!(err_text(settings::load(&path)).contains("settings format 9"));
    std::fs::write(&path, b"not json").unwrap();
    assert!(err_text(settings::load(&path)).contains("not a settings file"));
    // Preferences only: switching writes a good file over it.
    let (_, p) = settings::switch_network(&path, app, MAIN).unwrap();
    assert_eq!(p.net_name(), "main");
    assert_eq!(settings::load(&path).unwrap().network, "main");
    assert!(settings::parse_network("mainnet").is_ok() && settings::parse_network("Main").is_err());
}
