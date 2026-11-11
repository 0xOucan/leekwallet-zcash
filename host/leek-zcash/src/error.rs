//! One error type for every call, with stable codes.
//!
//! Wallets forward error text to logs and analytics, so a [`LeekError`]'s
//! message is a fixed sentence per variant: it never carries an amount, an
//! address, a key, PCZT bytes, or the device's own free-text message (which
//! can name an address). The firmware code, when there was one, is kept as a
//! number in [`LeekError::Device`] only for codes this crate does not know.

use std::fmt;

use leek_device::device::{
    is_code, DeviceError, ERR_BOARD_NETWORK, ERR_BUSY, ERR_NOT_UNLOCKED, ERR_NO_WALLET, ERR_SESSION, ERR_USER_REJECTED,
    ERR_USER_TIMEOUT, ERR_ZSIGN_BASE,
};

use crate::account::Network;

/// What a wallet can do about an error.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Recovery {
    /// Ask again over the same link and session (perhaps after the user did
    /// something on the board: unlock it, switch its network, answer).
    RetrySameLink,
    /// The session is gone or its state is unknown: reconnect and pair again.
    Reconnect,
    /// Retrying the same request cannot succeed: the PCZT, the account or the
    /// address is the problem.
    DoNotRetry,
}

/// The part of a request the board (or this crate) does not support.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Unsupported {
    /// A pool the board does not spend from, or a bundle shape it refuses
    /// (non-P2PKH transparent input, a sighash type other than ALL, ...).
    Pool,
    /// A transaction version the board does not sign.
    Version,
    /// A Sapling spend or output: the board has no Sapling key.
    Sapling,
    /// A PCZT another signer already partly signed with value at stake.
    Multistep,
    /// A consensus branch ID outside the board's list.
    Branch,
    /// A network other than Mainnet and Testnet.
    Network,
}

/// Every error the LeekWallet Zcash kit returns.
///
/// [`code`](Self::code) is stable across releases and bindings;
/// [`recovery`](Self::recovery) says whether to retry over the same link,
/// reconnect, or give up on the request.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum LeekError {
    // ---- link (1xx)
    /// No board on this link.
    NotFound,
    /// The platform refused access to the board (USB permission, ...).
    PermissionDenied,
    /// The link dropped mid-request.
    Disconnected,
    /// The board did not answer in time.
    Timeout,
    /// The request was cancelled by the caller.
    Cancelled,

    // ---- session and board state (2xx)
    /// The pairing handshake failed (a commitment that does not open, a
    /// degenerate key): possibly a relay. Pair again.
    PairingCodeMismatch,
    /// No session, or a reply failed authentication.
    SessionExpired,
    /// The board is busy with the user; the request was not acted on.
    Busy,
    /// The board is locked: the user enters the PIN on the board.
    Locked,
    /// The board holds no wallet (or its seed is not open yet).
    NoWallet,
    /// The board speaks another protocol version, or lacks a call.
    FirmwareTooOld,
    /// The board's reply was malformed or out of order.
    Protocol,
    /// The upload reached the board corrupted (offset, hash, incomplete).
    TransferCorrupted,
    /// The board failed internally (its two signer backends disagreed, a
    /// fresh signature did not verify, out of memory). Report it.
    DeviceInternal,

    // ---- the user (3xx)
    /// The user said no on the board.
    UserRejected,
    /// The user did not answer on the board in time.
    UserTimeout,

    // ---- account and network (4xx)
    /// The PCZT or the request is for the other network than the board's
    /// setting or the account's.
    WrongNetwork,
    /// The board refused a request for one network because its own setting
    /// (Settings → Network on the board) is `board`, the other one. The same
    /// code as [`WrongNetwork`](Self::WrongNetwork), 400; its message tells
    /// the user which way to switch, as every LeekWallet host words it.
    BoardNetwork {
        /// The board's network setting.
        board: Network,
    },
    /// The PCZT names another account (or another seed) than the one asked.
    WrongAccount,
    /// A spend or transparent input is not this account's.
    SpendNotOurs,
    /// The board says the address is not from its wallet.
    AddressNotOurs,
    /// The address has no receiver the board can check (only Sapling).
    AddressUncheckable,
    /// A spend lacks the derivation the board needs (run
    /// [`prepare_for_signer`](crate::prepare_for_signer)).
    MissingDerivation,

    // ---- the PCZT (5xx)
    /// Something the board does not support.
    Unsupported(Unsupported),
    /// More transparent inputs (or outputs, signatures) than the board holds.
    TooMany,
    /// The PCZT is larger than the board accepts, even in the compact view.
    TooLarge,
    /// The PCZT (or a field in it) does not parse.
    Parse,
    /// An unknown PCZT encoding version.
    PcztVersion,
    /// Values do not balance (outputs exceed inputs, or cv_net disagrees).
    ValueBalance,
    /// An output's commitment or user-facing address does not match its note.
    OutputMismatch,
    /// The PCZT lacks data the board needs to check it.
    MissingData,
    /// Two Orchard or Ironwood actions share one randomized verification
    /// key (`rk`): a malformed or hostile PCZT, since one signature could
    /// then stand for two actions.
    DuplicateRk,
    /// The signed transaction commits to another network upgrade's consensus
    /// branch than the next block's, as one signed before an activation does
    /// after it. No node takes it, so sending it again cannot help: build the
    /// transaction again and sign it again.
    WrongBranch,
    /// The signed transaction is past its expiry height. Build the
    /// transaction again and sign it again.
    Expired,
    /// The board refused the PCZT (0x0519): branch NU6.3 with an expiry that
    /// only an app without NU7's mainnet height builds. Update the app, then
    /// build the transaction again.
    AppOutdated,

    // ---- signatures (6xx)
    /// The `.zsig` names other bytes than the PCZT that was sent.
ZsigHashMismatch,
    /// The `.zsig` file is malformed.
    ZsigMalformed,
    /// A signature does not verify, or names an action or input it cannot
    /// sign (wrong pool, wrong index, already signed, a duplicate).
    SignatureInvalid,
    /// After applying the signatures a spend is still unsigned.
    SignatureMissing,
    /// The view sent to the board is a different transaction than the
    /// wallet's PCZT, or the board reviewed another sighash.
    SighashMismatch,

    // ---- stored app data (7xx)
    /// The app identifier is not lower-case ASCII reverse-DNS of 3 to 64
    /// bytes; the board would refuse it.
    InvalidAppId,
    /// A Storage Subkey label other than `db`, `ufvk` or `x-...`.
    InvalidStorageLabel,
    /// The stored object was sealed under another Storage Key: another seed
    /// or passphrase, app, account or network.
    StorageKeyMismatch,
    /// The stored object is malformed or was changed.
    StorageCorrupted,
    /// The Storage Key was already forgotten; ask the board again.
    StorageKeyForgotten,
    /// A seed outside ZIP 32's 32 to 252 bytes.
    InvalidSeed,
    /// Quick unlock was refused: the platform key is not hardware-backed and
    /// user-authenticated (a software key, or one usable without the user's
    /// biometric or device PIN). Do not offer quick unlock on this host.
    QuickUnlockNotSecure,
    /// The quick-unlock key on this host is gone, was invalidated (a new
    /// biometric enrolled, the device PIN removed) or gives another key.
    /// Unlock with the board, then set quick unlock up again.
    QuickUnlockInvalidated,
    /// The user cancelled (or failed) the platform's biometric or PIN prompt.
    QuickUnlockCancelled,
    /// The platform's secure hardware failed. Unlock with the board.
    QuickUnlockFailed,

    /// A firmware error code this crate does not know yet.
    Device {
        /// The firmware's code (docs/PROTOCOL.md §6).
        code: u16,
    },
}

impl LeekError {
    /// The stable numeric code: 1xx link, 2xx session and board, 3xx user,
    /// 4xx account and network, 5xx PCZT, 6xx signatures, 7xx stored app
    /// data, 9xx unknown.
    pub fn code(&self) -> u16 {
        use LeekError::*;
        match self {
            NotFound => 100,
            PermissionDenied => 101,
            Disconnected => 102,
            Timeout => 103,
            Cancelled => 104,
            PairingCodeMismatch => 200,
            SessionExpired => 201,
            Busy => 202,
            Locked => 203,
            NoWallet => 204,
            FirmwareTooOld => 205,
            Protocol => 206,
            TransferCorrupted => 207,
            DeviceInternal => 208,
            UserRejected => 300,
            UserTimeout => 301,
            WrongNetwork | BoardNetwork { .. } => 400,
            WrongAccount => 401,
            SpendNotOurs => 402,
            AddressNotOurs => 403,
            AddressUncheckable => 404,
            MissingDerivation => 405,
            Unsupported(u) => match u {
                self::Unsupported::Pool => 500,
                self::Unsupported::Version => 501,
                self::Unsupported::Sapling => 502,
                self::Unsupported::Multistep => 503,
                self::Unsupported::Branch => 504,
                self::Unsupported::Network => 505,
            },
            TooMany => 510,
            TooLarge => 511,
            Parse => 512,
            PcztVersion => 513,
            ValueBalance => 514,
            OutputMismatch => 515,
            MissingData => 516,
            DuplicateRk => 517,
            WrongBranch => 518,
            Expired => 519,
            AppOutdated => 520,
            ZsigHashMismatch => 600,
            ZsigMalformed => 601,
            SignatureInvalid => 602,
            SignatureMissing => 603,
            SighashMismatch => 604,
            InvalidAppId => 700,
            InvalidStorageLabel => 701,
            StorageKeyMismatch => 702,
            StorageCorrupted => 703,
            StorageKeyForgotten => 704,
            InvalidSeed => 705,
            QuickUnlockNotSecure => 706,
            QuickUnlockInvalidated => 707,
            QuickUnlockCancelled => 708,
            QuickUnlockFailed => 709,
            Device { .. } => 900,
        }
    }

    /// Whether to retry over the same link, reconnect, or not retry.
    pub fn recovery(&self) -> Recovery {
        use LeekError::*;
        use Recovery::*;
        match self {
            Busy | Locked | NoWallet | UserRejected | UserTimeout | WrongNetwork | BoardNetwork { .. } | TransferCorrupted
            | QuickUnlockCancelled => RetrySameLink,
            NotFound | PermissionDenied | Disconnected | Timeout | Cancelled | PairingCodeMismatch | SessionExpired
            | Protocol => Reconnect,
            _ => DoNotRetry,
        }
    }

    fn message(&self) -> &'static str {
        use LeekError::*;
        match self {
            NotFound => "no LeekWallet board found on this link",
            PermissionDenied => "access to the board was not granted",
            Disconnected => "the link to the board dropped",
            Timeout => "the board did not answer in time",
            Cancelled => "the request was cancelled",
            PairingCodeMismatch => "pairing with the board failed; pair again",
            SessionExpired => "the session with the board ended; reconnect and pair again",
            Busy => "the board is busy with the user; try again",
            Locked => "the board is locked; enter the PIN on the board",
            NoWallet => "the board has no wallet open",
            FirmwareTooOld => "the board's firmware does not support this; update it",
            Protocol => "the board sent a reply this host does not understand",
            TransferCorrupted => "the transaction reached the board corrupted; try again",
            DeviceInternal => "the board failed internally",
            UserRejected => "rejected on the board",
            UserTimeout => "no answer on the board in time",
            WrongNetwork => "the board or the transaction is on the other network",
            // leek_device::device::wrong_network_message, word for word
            // (a test keeps them equal): what the Android app, the
            // companion and the Chrome wallet show.
            BoardNetwork { board: Network::Test } => {
                "The board is set to Testnet, but this app is on Mainnet. Switch the board to Mainnet in \
                 Settings → Network, or use the Testnet app."
            }
            BoardNetwork { board: Network::Main } => {
                "The board is set to Mainnet, but this app is on Testnet. Switch the board to Testnet in \
                 Settings → Network, or use the Mainnet app."
            }
            WrongAccount => "the transaction is for another account or seed than the board's",
            SpendNotOurs => "the transaction spends funds this account does not hold",
            AddressNotOurs => "the board says this address is not from its wallet",
            AddressUncheckable => "this address has no receiver the board can check",
            MissingDerivation => "a spend lacks its key derivation; prepare the transaction for the signer",
            Unsupported(u) => match u {
                self::Unsupported::Pool => "the transaction uses a pool or input type the board does not sign",
                self::Unsupported::Version => "the board does not sign this transaction version",
                self::Unsupported::Sapling => "the board does not sign transactions with Sapling spends or outputs",
                self::Unsupported::Multistep => "the transaction was already partly signed by another signer",
                self::Unsupported::Branch => "the board does not sign for this network upgrade",
                self::Unsupported::Network => "only Mainnet and Testnet are supported",
            },
            TooMany => "the transaction has more inputs or outputs than the board holds",
            TooLarge => "the transaction is too large for the board",
            Parse => "the transaction does not parse",
            PcztVersion => "unknown PCZT encoding version",
            ValueBalance => "the transaction's values do not balance",
            OutputMismatch => "an output does not match its commitment or its displayed address",
            MissingData => "the transaction lacks data the board needs to check it",
            DuplicateRk => "two spends in the transaction share one verification key",
            WrongBranch => "the transaction was signed for another network upgrade than the chain's next block; build and sign it again",
            Expired => "the transaction is past its expiry height; build and sign it again",
            AppOutdated => leek_device::device::APP_UPDATE_TEXT,
            ZsigHashMismatch => "the signatures were made for a different PCZT",
            ZsigMalformed => "the signature file is malformed",
            SignatureInvalid => "a signature from the board does not verify",
            SignatureMissing => "a spend is still unsigned after applying the board's signatures",
            SighashMismatch => "the board signed a different transaction than this wallet's",
            InvalidAppId => "the app identifier must be lower-case reverse-DNS, such as com.example.wallet",
            InvalidStorageLabel => "unknown storage label",
            StorageKeyMismatch => "the stored data was sealed for another seed, app, account or network",
            StorageCorrupted => "the stored data is damaged or was changed",
            StorageKeyForgotten => "the storage key was already forgotten; ask the board again",
            InvalidSeed => "the seed is too short or too long",
            QuickUnlockNotSecure => "quick unlock needs a hardware-backed key that asks for the user's biometric or device PIN",
            QuickUnlockInvalidated => "quick unlock no longer works on this device; unlock with the board",
            QuickUnlockCancelled => "the biometric or device PIN prompt was cancelled",
            QuickUnlockFailed => "the device's secure hardware failed; unlock with the board",
            Device { .. } => "the board refused the request",
        }
    }
}

impl fmt::Display for LeekError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            LeekError::Device { code } => write!(f, "{} (code 0x{code:04X})", self.message()),
            _ => f.write_str(self.message()),
        }
    }
}

impl std::error::Error for LeekError {}

/// Which call an error came from: a few firmware codes mean different
/// things for an address check and for signing.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Ctx {
    Sign,
    ShowAddress,
    Other,
}

/// A firmware error code (docs/PROTOCOL.md §6, `ZS_E_*`, `ZSIGN_E_*`).
pub(crate) fn from_code(code: u64, ctx: Ctx) -> LeekError {
    use LeekError::*;
    match code {
        0x0001 => Protocol,
        0x0002 => FirmwareTooOld,
        ERR_NOT_UNLOCKED => Locked,
        0x0101 => PermissionDenied,
        ERR_USER_REJECTED => UserRejected,
        ERR_USER_TIMEOUT => UserTimeout,
        ERR_NO_WALLET => NoWallet,
        ERR_SESSION => SessionExpired,
        ERR_BUSY => Busy,
        ERR_BOARD_NETWORK => WrongNetwork,
        c if c & 0xFF00 == ERR_ZSIGN_BASE => match c & 0xFF {
            // ZS_E_* (zsign_types.h)
            10 => Parse,
            11 => Unsupported(self::Unsupported::Branch),
            12 => SpendNotOurs,
            13 => OutputMismatch,
            14 if ctx == Ctx::ShowAddress => AddressNotOurs,
            14 => OutputMismatch,
            15 => ValueBalance,
            16 if ctx == Ctx::ShowAddress => AddressUncheckable,
            16 => Unsupported(self::Unsupported::Pool),
            17 => WrongNetwork,
            18 => TooMany,
            19 => MissingData,
            // A dummy spending key in the PCZT: Signers must refuse it.
            20 => Parse,
            21..=23 => DeviceInternal,
            // ZS_E_DUPLICATE_RK: two actions, both pools together, share an rk.
            24 => DuplicateRk,
            // ZS_E_APP_OUTDATED: NU6.3, with an expiry past NU7 + 40.
            25 => AppOutdated,
            // ZSIGN_E_* (zcash-sign.h)
            30 => Protocol,
            31 => TooLarge,
            32..=34 => TransferCorrupted,
            35 | 36 | 37 | 39 => DeviceInternal,
            38 => Locked,
            _ => Device { code: code as u16 },
        },
        _ => Device { code: code as u16 },
    }
}

/// Classifies an error from `leek-device`. Its typed [`DeviceError`] is
/// mapped by code; its other errors are `anyhow` text (the transport error
/// is formatted into it), recognised by their fixed wording.
pub(crate) fn from_device(e: &anyhow::Error, ctx: Ctx) -> LeekError {
    if let Some(d) = e.downcast_ref::<DeviceError>() {
        return from_code(d.code, ctx);
    }
    // TODO: once leek-device keeps TransportError typed in its anyhow chain,
    // match on it instead of the wording.
    let text = e.to_string();
    let t = text.as_str();
    if t.starts_with("send: ") || t.starts_with("receive: ") {
        LeekError::Disconnected
    } else if t.contains("did not answer") {
        LeekError::Timeout
    } else if t.contains("failed authentication") || t == "not paired" {
        LeekError::SessionExpired
    } else if t.contains("different protocol version") {
        LeekError::FirmwareTooOld
    } else if t.contains("commitment does not open") || t.contains("degenerate shared secret") {
        LeekError::PairingCodeMismatch
    } else if t.contains("pairing was not confirmed") {
        LeekError::UserTimeout
    } else if t.contains("was not unlocked in time") {
        LeekError::Locked
    } else if ctx == Ctx::ShowAddress && t.starts_with("the device confirmed") {
        // Old firmware ignores the app's address and shows its default one.
        LeekError::AddressNotOurs
    } else {
        LeekError::Protocol
    }
}

/// [`from_device`] for a request that named the network `asked`: the
/// board's refusal for its own network setting (0x0528) becomes
/// [`LeekError::BoardNetwork`], which says which way to switch.
pub(crate) fn from_device_on(e: &anyhow::Error, ctx: Ctx, asked: Network) -> LeekError {
    match from_device(e, ctx) {
        LeekError::WrongNetwork if is_code(e, ERR_BOARD_NETWORK) => LeekError::BoardNetwork { board: asked.other() },
        l => l,
    }
}

/// Any error from `leek-device` (for example from `Device::pair`) as a
/// [`LeekError`].
pub fn classify(e: &anyhow::Error) -> LeekError {
    from_device(e, Ctx::Other)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn all() -> Vec<LeekError> {
        use LeekError::*;
        let mut v = vec![
            NotFound, PermissionDenied, Disconnected, Timeout, Cancelled, PairingCodeMismatch, SessionExpired, Busy,
            Locked, NoWallet, FirmwareTooOld, Protocol, TransferCorrupted, DeviceInternal, UserRejected, UserTimeout,
            WrongNetwork, WrongAccount, SpendNotOurs, AddressNotOurs, AddressUncheckable, MissingDerivation, TooMany,
            TooLarge, Parse, PcztVersion, ValueBalance, OutputMismatch, MissingData, DuplicateRk, WrongBranch, AppOutdated, Expired, ZsigHashMismatch, ZsigMalformed,
            SignatureInvalid, SignatureMissing, SighashMismatch, InvalidAppId, InvalidStorageLabel,
            StorageKeyMismatch, StorageCorrupted, StorageKeyForgotten, InvalidSeed, QuickUnlockNotSecure,
            QuickUnlockInvalidated, QuickUnlockCancelled, QuickUnlockFailed, Device { code: 0x0599 },
            BoardNetwork { board: Network::Main }, BoardNetwork { board: Network::Test },
        ];
        use super::Unsupported as U;
        for u in [U::Pool, U::Version, U::Sapling, U::Multistep, U::Branch, U::Network] {
            v.push(Unsupported(u));
        }
        v
    }

    #[test]
    fn codes_are_unique() {
        // BoardNetwork shares WrongNetwork's 400: one code, a more precise text.
        let mut codes: Vec<u16> =
            all().iter().filter(|e| !matches!(e, LeekError::BoardNetwork { .. })).map(LeekError::code).collect();
        let n = codes.len();
        codes.sort_unstable();
        codes.dedup();
        assert_eq!(codes.len(), n);
    }

    #[test]
    fn messages_carry_no_data() {
        for e in all() {
            let m = e.to_string();
            assert!(!m.is_empty());
            // No address prefixes, no hex blobs, no digits beyond the code.
            for p in ["u1", "utest1", "t1", "tm", "uview", "zs1"] {
                assert!(!m.split_whitespace().any(|w| w.starts_with(p) && w.len() > 20), "{m}");
            }
            assert!(!m.chars().any(|c| c.is_ascii_digit()) || matches!(e, LeekError::Device { .. }), "{m}");
        }
    }

    #[test]
    fn firmware_codes_map() {
        assert_eq!(from_code(0x0200, Ctx::Sign), LeekError::UserRejected);
        assert_eq!(from_code(0x0401, Ctx::Sign), LeekError::Busy);
        assert_eq!(from_code(0x0528, Ctx::Other), LeekError::WrongNetwork);
        assert_eq!(from_code(0x050C, Ctx::Sign), LeekError::SpendNotOurs);
        assert_eq!(from_code(0x050E, Ctx::ShowAddress), LeekError::AddressNotOurs);
        assert_eq!(from_code(0x050E, Ctx::Sign), LeekError::OutputMismatch);
        assert_eq!(from_code(0x0510, Ctx::ShowAddress), LeekError::AddressUncheckable);
        assert_eq!(from_code(0x0511, Ctx::Sign), LeekError::WrongNetwork);
        assert_eq!(from_code(0x0512, Ctx::Sign), LeekError::TooMany);
        assert_eq!(from_code(0x0521, Ctx::Sign), LeekError::TransferCorrupted);
        assert_eq!(from_code(0x0518, Ctx::Sign), LeekError::DuplicateRk);
        assert_eq!(from_code(0x0519, Ctx::Sign), LeekError::AppOutdated);
        assert_eq!(LeekError::AppOutdated.code(), 520);
        assert_eq!(LeekError::AppOutdated.recovery(), Recovery::DoNotRetry);
        assert_eq!(LeekError::DuplicateRk.code(), 517);
        assert_eq!(LeekError::DuplicateRk.recovery(), Recovery::DoNotRetry);
        assert_eq!((LeekError::WrongBranch.code(), LeekError::WrongBranch.recovery()), (518, Recovery::DoNotRetry));
        assert_eq!((LeekError::Expired.code(), LeekError::Expired.recovery()), (519, Recovery::DoNotRetry));
        assert_eq!(from_code(0x05FE, Ctx::Sign), LeekError::Device { code: 0x05FE });
        assert_eq!(LeekError::Busy.recovery(), Recovery::RetrySameLink);
        assert_eq!(LeekError::SessionExpired.recovery(), Recovery::Reconnect);
        assert_eq!(LeekError::SpendNotOurs.recovery(), Recovery::DoNotRetry);
    }

    #[test]
    fn device_text_never_reaches_the_message() {
        let e: anyhow::Error = DeviceError { code: 0x050E, message: "utest1secretaddress".into() }.into();
        let l = from_device(&e, Ctx::ShowAddress);
        assert_eq!(l, LeekError::AddressNotOurs);
        assert!(!l.to_string().contains("utest1"));
        let e = anyhow::anyhow!("the device confirmed utest1abc, but this wallet expects utest1def");
        assert_eq!(from_device(&e, Ctx::ShowAddress), LeekError::AddressNotOurs);
        assert_eq!(from_device(&anyhow::anyhow!("receive: broken pipe"), Ctx::Sign), LeekError::Disconnected);
        assert_eq!(
            from_device(&anyhow::anyhow!("the device did not answer (is it connected?)"), Ctx::Sign),
            LeekError::Timeout
        );
        // Context added by leek-device keeps the DeviceError underneath.
        let e = anyhow::Error::from(DeviceError { code: 0x0528, message: "x".into() }).context("The board is set to");
        assert_eq!(classify(&e), LeekError::WrongNetwork);
    }

    #[test]
    fn the_board_network_refusal_says_which_way_to_switch() {
        use leek_device::device::wrong_network_message;
        let refusal = || anyhow::Error::from(DeviceError { code: ERR_BOARD_NETWORK, message: "x".into() });
        for asked in [Network::Main, Network::Test] {
            let e = from_device_on(&refusal(), Ctx::Other, asked);
            assert_eq!(e, LeekError::BoardNetwork { board: asked.other() });
            assert_eq!(e.to_string(), wrong_network_message(asked.as_str()), "the hosts' sentence");
            assert_eq!((e.code(), e.recovery()), (400, Recovery::RetrySameLink));
            // Through leek-device's own context too.
            let ctx = refusal().context(wrong_network_message(asked.as_str()));
            assert_eq!(from_device_on(&ctx, Ctx::Sign, asked), e);
        }
        // The PCZT's network against the request (ZS_E_NETWORK) is not the
        // board's setting: it keeps the general text.
        let e = anyhow::Error::from(DeviceError { code: 0x0511, message: "x".into() });
        assert_eq!(from_device_on(&e, Ctx::Sign, Network::Test), LeekError::WrongNetwork);
        // Without a network, as for `classify`: the general text.
        assert_eq!(classify(&refusal()), LeekError::WrongNetwork);
    }
}
