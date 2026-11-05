//! Typed wrappers over the board's account calls: `zGetAccount`,
//! `zGetSeedFingerprint`, `zShowAddress`, `zSetAccountMeta`, `getFeatures` + `zGetFeatures`.

use std::time::Duration;

use leek_device::device::{self, Device, Value};
use zcash_address::unified::{self, Encoding};
use zcash_protocol::consensus::NetworkType;

use crate::error::{from_device, Ctx, LeekError};

/// The key source a wallet records for an account imported from the board
/// (`import_account_ufvk(.., key_source)`).
pub const KEY_SOURCE: &str = "leekwallet";

/// The transparent inputs the board holds in one PCZT (`ZS_MAX_T_INPUTS`).
pub const MAX_TRANSPARENT_INPUTS: usize = 16;

/// Branch IDs signed by firmware that predates reporting them: NU6.3 and NU7.
pub const KNOWN_BRANCH_IDS: [u32; 2] = [0x37a5_165b, 0x7719_0ad9];

/// The largest PCZT the board accepts (`ZCASH_PCZT_MAX`).
pub const MAX_PCZT_BYTES: usize = 512 * 1024;

const SHORT: Duration = Duration::from_secs(10);

/// A Zcash network the board serves.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Network {
    Main,
    Test,
}

impl Network {
    /// The wire name: `"main"` or `"test"`.
    pub fn as_str(self) -> &'static str {
        match self {
            Network::Main => "main",
            Network::Test => "test",
        }
    }

    /// The ZIP 32 / SLIP 44 coin type: 133 or 1.
    pub fn coin_type(self) -> u32 {
        match self {
            Network::Main => 133,
            Network::Test => 1,
        }
    }

    /// The network with this coin type.
    pub fn from_coin_type(coin_type: u32) -> Option<Self> {
        match coin_type {
            133 => Some(Network::Main),
            1 => Some(Network::Test),
            _ => None,
        }
    }

    pub(crate) fn from_type(t: NetworkType) -> Result<Self, LeekError> {
        match t {
            NetworkType::Main => Ok(Network::Main),
            NetworkType::Test => Ok(Network::Test),
            NetworkType::Regtest => Err(LeekError::Unsupported(crate::Unsupported::Network)),
        }
    }

    pub(crate) fn params(self) -> zcash_protocol::consensus::Network {
        match self {
            Network::Main => zcash_protocol::consensus::Network::MainNetwork,
            Network::Test => zcash_protocol::consensus::Network::TestNetwork,
        }
    }
}

/// A board account, as `zGetAccount` exports it: what a wallet imports
/// (`import_account_ufvk` with `Zip32Derivation(seed_fingerprint, index)`
/// and [`KEY_SOURCE`]).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Account {
    /// The ZIP 32 account index.
    pub index: u32,
    pub network: Network,
    /// The ZIP 32 seed fingerprint of the board's open seed.
    pub seed_fingerprint: [u8; 32],
    /// Orchard + P2PKH unified full viewing key (no Sapling).
    pub ufvk: String,
    /// The default unified address.
    pub ua: String,
    /// The transparent address at index 0.
    pub t_address: String,
    /// The birthday height the board holds for this account, if any.
    pub birthday: Option<u32>,
}

impl Account {
    /// The account's identity for [`preflight`](crate::preflight).
    pub fn meta(&self) -> AccountMeta {
        AccountMeta {
            seed_fingerprint: self.seed_fingerprint,
            index: self.index,
            network: self.network,
            ufvk: Some(self.ufvk.clone()),
        }
    }
}

/// An account's identity: (seed fingerprint, index, network), and, when the
/// wallet has it, the UFVK, which lets [`preflight`](crate::preflight) check
/// that every spend and transparent input really derives from it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AccountMeta {
    pub seed_fingerprint: [u8; 32],
    pub index: u32,
    pub network: Network,
    pub ufvk: Option<String>,
}

/// What the board's firmware and signer support.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Features {
    /// The session protocol version this host speaks with the board.
    pub protocol: u64,
    /// The firmware version string, if the board reported it.
    pub firmware: Option<String>,
    /// The board model, if reported.
    pub model: Option<String>,
    /// Pools the signer spends from: "orchard", "ironwood", "transparent".
    pub spend_pools: Vec<String>,
    /// Consensus branch IDs both signer backends accept.
    pub branch_ids: Vec<u32>,
    /// Transaction versions the signer accepts.
    pub tx_versions: Vec<u32>,
    /// The largest PCZT upload.
    pub max_pczt_bytes: usize,
    /// Transparent inputs per PCZT.
    pub max_transparent_inputs: usize,
    /// Whether `zShowAddress` is available.
    pub show_address: bool,
    /// The ZIP number the board derives App Storage Keys under
    /// (`zGetStorageKey`), or `None` if its firmware has no such call. An
    /// app offers "unlock with the board" only when this is
    /// [`STORAGE_ZIP_NUMBER`](crate::STORAGE_ZIP_NUMBER).
    pub storage_key: Option<u16>,
    /// The firmware's `fwCompat` from `zGetFeatures`; `None` when it reports
    /// none, as firmware from before this field does.
    pub fw_compat: Option<u32>,
}

impl Features {
    /// True when the board's firmware is older than
    /// [`MIN_FIRMWARE_COMPAT`]: suggest an update with [`BOARD_UPDATE_TEXT`].
    /// A suggestion and not a refusal: such a board still signs what its
    /// signers accept. Only meaningful for what [`get_features`] returned.
    pub fn firmware_outdated(&self) -> bool {
        device::firmware_outdated(self.fw_compat)
    }
}

impl Default for Features {
    /// What the current firmware supports, for a host that has not asked
    /// (an SD-card flow, or a preflight before connecting).
    fn default() -> Self {
        Features {
            protocol: device::PROTOCOL_VERSION,
            firmware: None,
            model: None,
            spend_pools: vec!["orchard".into(), "ironwood".into(), "transparent".into()],
            branch_ids: KNOWN_BRANCH_IDS.to_vec(),
            tx_versions: vec![6],
            max_pczt_bytes: MAX_PCZT_BYTES,
            max_transparent_inputs: MAX_TRANSPARENT_INPUTS,
            show_address: true,
            storage_key: None,
            fw_compat: None,
        }
    }
}

/// The result of an on-board address check.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AddressCheck {
    /// The board derived this address from its keys and the user confirmed
    /// it matches.
    Verified,
    /// The user said on the board that it does not match.
    Rejected,
    /// The board says it is not from its wallet. Do not receive to it.
    NotOurs,
    /// The address has a receiver the board cannot check (Sapling); see
    /// [`to_checkable_ua`](crate::to_checkable_ua).
    Unsupported,
}

fn text(v: &Value, k: &str) -> Result<String, LeekError> {
    device::get_text(v, k).map_err(|_| LeekError::Protocol)
}

fn uints(v: &Value, k: &str) -> Option<Vec<u32>> {
    match device::get(v, k) {
        Some(Value::Array(a)) => a
            .iter()
            .map(|x| match x {
                Value::Integer(i) => u32::try_from(i128::from(*i)).ok(),
                _ => None,
            })
            .collect(),
        _ => None,
    }
}

fn texts(v: &Value, k: &str) -> Option<Vec<String>> {
    match device::get(v, k) {
        Some(Value::Array(a)) => a
            .iter()
            .map(|x| match x {
                Value::Text(t) => Some(t.clone()),
                _ => None,
            })
            .collect(),
        _ => None,
    }
}

/// Where [`get_account_with`] (or [`get_storage_key_with`](crate::get_storage_key_with)) is.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum AccountProgress {
    /// The request is on the board. For [`get_account_with`], unless the
    /// user already agreed in this session (same account, network and seed),
    /// the board asks "Share view key?"; for
    /// [`get_storage_key_with`](crate::get_storage_key_with) it always asks
    /// "Unlock app data?". It answers only after OK there, waiting up to
    /// 120 s. Tell the user to confirm on the board. The board does not say
    /// whether it is asking, so this comes for every call.
    ConfirmOnDevice,
}

/// `zGetAccount`: the account's UFVK, address, t-address, seed fingerprint
/// and birthday. Derives keys on the board (seconds on the S3), and the
/// board releases the UFVK only after a press (see [`get_account_with`]).
pub fn get_account(device: &mut Device, index: u32, network: Network) -> Result<Account, LeekError> {
    get_account_with(device, index, network, &|_| {})
}

/// [`get_account`], telling `progress` when the board may be asking the
/// user. A no on the board is [`LeekError::UserRejected`]; no answer in its
/// 120 s window, [`LeekError::UserTimeout`].
pub fn get_account_with(
    device: &mut Device,
    index: u32,
    network: Network,
    progress: &dyn Fn(AccountProgress),
) -> Result<Account, LeekError> {
    progress(AccountProgress::ConfirmOnDevice);
    let r = device.z_get_account(index, network.as_str()).map_err(|e| from_device(&e, Ctx::Other))?;
    if text(&r, "network")? != network.as_str() {
        return Err(LeekError::WrongNetwork);
    }
    let seed_fingerprint: [u8; 32] = device::get_bytes(&r, "seedFingerprint")
        .ok()
        .and_then(|b| b.try_into().ok())
        .ok_or(LeekError::Protocol)?;
    let ufvk = text(&r, "ufvk")?;
    // The UFVK must be for the network asked.
    match unified::Ufvk::decode(&ufvk) {
        Ok((t, _, _)) if Network::from_type(t)? == network => {}
        Ok(_) => return Err(LeekError::WrongNetwork),
        Err(_) => return Err(LeekError::Protocol),
    }
    let birthday = device::get_uint(&r, "birthday").ok().map(|b| u32::try_from(b).map_err(|_| LeekError::Protocol)).transpose()?;
    Ok(Account {
        index,
        network,
        seed_fingerprint,
        ufvk,
        ua: text(&r, "ua")?,
        t_address: text(&r, "tAddress")?,
        birthday,
    })
}

/// `zGetSeedFingerprint`: the ZIP 32 seed fingerprint of the seed open on
/// the board (passphrase applied), with no question on the board. What a
/// wallet uses to recognise the board on every connect (the same 32 bytes
/// as [`Account::seed_fingerprint`]); [`get_account`] is for when the UFVK
/// is needed, the first import, and asks "Share view key?".
///
/// The fingerprint is a stable identifier of the seed: every paired host
/// reads the same bytes, across accounts, networks and sessions. `network`,
/// when given, is checked against the board's setting
/// ([`LeekError::WrongNetwork`]) and does not change the answer. Firmware
/// without the call is [`LeekError::FirmwareTooOld`]; fall back to
/// [`get_account`] there.
pub fn seed_fingerprint(device: &mut Device, network: Option<Network>) -> Result<[u8; 32], LeekError> {
    device.z_get_seed_fingerprint(network.map(Network::as_str)).map_err(|e| {
        let unknown = e.downcast_ref::<device::DeviceError>().is_some_and(|d| d.code == 0x0001);
        if unknown { LeekError::FirmwareTooOld } else { from_device(&e, Ctx::Other) }
    })
}

/// `zShowAddress`: the board derives the account's address, checks that
/// `ua` is one of its own, pages it on its screen and asks the user whether
/// it matches. Errors are for the link and the board's state; the four
/// answers are [`AddressCheck`].
pub fn show_address(device: &mut Device, index: u32, network: Network, ua: &str) -> Result<AddressCheck, LeekError> {
    match device.z_show_address(index, network.as_str(), ua) {
        Ok(()) => Ok(AddressCheck::Verified),
        Err(e) => match from_device(&e, Ctx::ShowAddress) {
            LeekError::UserRejected => Ok(AddressCheck::Rejected),
            LeekError::AddressNotOurs => Ok(AddressCheck::NotOurs),
            LeekError::AddressUncheckable => Ok(AddressCheck::Unsupported),
            other => Err(other),
        },
    }
}

/// `zSetAccountMeta`: record the account's birthday on the board. True if
/// it is kept in flash (a stored wallet with no passphrase), false if only
/// until the board locks.
pub fn set_birthday(device: &mut Device, index: u32, network: Network, height: u32) -> Result<bool, LeekError> {
    device.z_set_birthday(index, network.as_str(), height).map_err(|e| from_device(&e, Ctx::Other))
}

pub use leek_device::device::{BOARD_UPDATE_TEXT, MIN_FIRMWARE_COMPAT};

/// `getFeatures` and `zGetFeatures`: the firmware and what its signer
/// accepts. Fields the firmware does not report keep their
/// [`Features::default`] value.
pub fn get_features(device: &mut Device) -> Result<Features, LeekError> {
    let fw = device.call("getFeatures", vec![], SHORT).map_err(|e| from_device(&e, Ctx::Other))?;
    if device::get_text(&fw, "chain").ok().as_deref() != Some("zcash") {
        // Not the Zcash firmware.
        return Err(LeekError::FirmwareTooOld);
    }
    let z = device.call("zGetFeatures", vec![], SHORT).map_err(|e| {
        let l = from_device(&e, Ctx::Other);
        if l == LeekError::Protocol { LeekError::FirmwareTooOld } else { l }
    })?;
    let mut f = Features {
        firmware: device::get_text(&fw, "firmware").ok(),
        model: device::get_text(&fw, "model").ok(),
        ..Features::default()
    };
    if let Some(v) = texts(&z, "spendPools") {
        f.spend_pools = v;
    }
    if let Some(v) = uints(&z, "branchIds") {
        f.branch_ids = v;
    }
    if let Some(v) = uints(&z, "txVersions") {
        f.tx_versions = v;
    }
    if let Ok(n) = device::get_uint(&z, "maxPczt") {
        f.max_pczt_bytes = n as usize;
    }
    f.show_address = device::get_uint(&z, "showAddress").unwrap_or(0) == 1;
    f.storage_key = device::get_uint(&z, "storageKey").ok().and_then(|n| u16::try_from(n).ok());
    f.fw_compat = device::get_uint(&z, "fwCompat").ok().and_then(|n| u32::try_from(n).ok());
    Ok(f)
}
