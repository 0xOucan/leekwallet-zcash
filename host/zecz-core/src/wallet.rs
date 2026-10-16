//! The watch-only wallet: one data directory, one network, one account.
//!
//! These are the operations behind `zecz init`, `sync`, `tip`, `balance`,
//! `address`, `send` and `finish`, as typed functions that return data and
//! never print. Long steps report progress through callbacks, so the CLI
//! and the desktop app can each show it their own way.
//!
//! A data directory holds `wallet.sqlite`, a `network` marker that binds it
//! to one network, `pending/` (the full PCZT of each send, named by the
//! SHA-256 of the signer view handed to the device) and `sent/` (each raw
//! transaction `finish` built).

use std::convert::Infallible;
use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{anyhow, bail, Context, Result};
use pczt::roles::signer::Signer;
use pczt::Pczt;
use rand::{rand_core::UnwrapErr, rngs::SysRng};
use zcash_client_backend::data_api::wallet::{
    create_pczt_from_proposal, extract_and_store_transaction_from_pczt, input_selection::GreedyInputSelector,
    input_selection::SpendPolicy, propose_transfer, redact_pczt_for_signer, ConfirmationsPolicy, SignerView,
};
use zcash_client_backend::data_api::{Account as _, AccountPurpose, WalletRead, WalletWrite, Zip32Derivation};
use zcash_client_backend::fees::{standard::SingleOutputChangeStrategy, DustOutputPolicy, StandardFeeRule};
use zcash_client_backend::proto::service::{BlockId, ChainSpec, Empty, RawTransaction};
use zcash_client_backend::util::SystemClock;
use zcash_client_backend::wallet::OvkPolicy;
use zcash_client_sqlite::{wallet::init::init_wallet_db, AccountUuid, ReceivedNoteId, WalletDb};
use zcash_keys::address::Address;
use zcash_keys::keys::{ReceiverRequirement, UnifiedAddressRequest, UnifiedFullViewingKey};
use zcash_primitives::transaction::builder::BundlePadding;
use zcash_protocol::consensus::{BlockHeight, NetworkConstants, NetworkType, NetworkUpgrade, Parameters};
use zcash_protocol::memo::{Memo, MemoBytes};
use zcash_protocol::value::Zatoshis;
use zcash_protocol::{PoolType, ShieldedPool};
use zip321::{Payment, TransactionRequest};
use zip32::fingerprint::SeedFingerprint;

use crate::authorize::{apply_zsig, prove_with, ProveEvent};
use crate::keys::DevAccount;
use crate::lwd::{self, MemBlockCache, ScannedRange};
use crate::zsig::{self, Zsig};

pub use zcash_client_backend::data_api::AccountBirthday;
pub use zcash_protocol::consensus::Network;

type Db = WalletDb<rusqlite::Connection, Network, SystemClock, UnwrapErr<SysRng>>;

/// The default lightwalletd endpoint for a network.
pub fn default_server(network: Network) -> &'static str {
    match network {
        Network::TestNetwork => "https://testnet.zec.rocks:443",
        Network::MainNetwork => "https://zec.rocks:443",
    }
}

/// The short network name used in the `network` marker, on the device and
/// in messages: "test" or "main".
pub fn network_name(network: Network) -> &'static str {
    match network.network_type() {
        NetworkType::Test => "test",
        NetworkType::Main => "main",
        NetworkType::Regtest => "regtest",
    }
}

/// Where a wallet lives and which chain it follows.
#[derive(Clone, Debug)]
pub struct Profile {
    pub network: Network,
    pub data_dir: PathBuf,
    /// lightwalletd gRPC endpoint.
    pub server: String,
}

impl Profile {
    /// A profile; `server` defaults to [`default_server`] for the network.
    pub fn new(network: Network, data_dir: impl Into<PathBuf>, server: Option<String>) -> Self {
        Profile { network, data_dir: data_dir.into(), server: server.unwrap_or_else(|| default_server(network).into()) }
    }

    pub fn net_name(&self) -> &'static str {
        network_name(self.network)
    }

    /// The ZIP 32 coin type (1 on testnet, 133 on mainnet).
    pub fn coin_type(&self) -> u32 {
        self.network.network_type().coin_type()
    }

    pub fn db_path(&self) -> PathBuf {
        self.data_dir.join("wallet.sqlite")
    }

    pub fn pending_dir(&self) -> PathBuf {
        self.data_dir.join("pending")
    }

    pub fn sent_dir(&self) -> PathBuf {
        self.data_dir.join("sent")
    }

    /// Whether the data directory already holds a wallet (for any network).
    pub fn exists(&self) -> bool {
        self.data_dir.join("network").exists()
    }
}

/// The account to import: a UFVK exported by the device and where it came
/// from.
#[derive(Clone, Debug)]
pub struct AccountSpec {
    /// Unified full viewing key (Orchard + P2PKH, no Sapling).
    pub ufvk: String,
    /// ZIP 32 seed fingerprint of the device seed.
    pub seed_fingerprint: [u8; 32],
    /// ZIP 32 account index.
    pub account_index: u32,
    /// Account name in the wallet database.
    pub name: String,
}

impl AccountSpec {
    /// DEV ONLY: the spec for an account derived on the host from a
    /// mnemonic (as `zecz init-dev` does).
    pub fn from_dev_account(network: Network, acct: &DevAccount, name: &str) -> Result<Self> {
        Ok(AccountSpec {
            ufvk: acct.ufvk.encode(&network)?,
            seed_fingerprint: acct.seed_fingerprint.to_bytes(),
            account_index: u32::from(acct.account),
            name: name.to_string(),
        })
    }
}

/// The addresses of an account at its default diversifier index.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Addresses {
    /// Unified address with Orchard and P2PKH receivers (the device's).
    pub unified: String,
    /// The diversifier index of `unified`.
    pub diversifier_index: u128,
    /// The P2PKH receiver of `unified`, encoded on its own.
    pub transparent: Option<String>,
    /// Unified address with only an Orchard receiver.
    pub orchard_only: String,
}

/// What [`Wallet::init`] did.
#[derive(Clone, Debug)]
pub enum InitOutcome {
    /// The same UFVK was already imported; nothing changed.
    AlreadyImported { addresses: Addresses },
    Imported(ImportedAccount),
}

#[derive(Clone, Debug)]
pub struct ImportedAccount {
    pub name: String,
    /// The account's UUID in the wallet database.
    pub uuid: String,
    pub birthday: u32,
    pub seed_fingerprint: [u8; 32],
    pub account_index: u32,
    pub addresses: Addresses,
}

impl ImportedAccount {
    /// The short seed fingerprint the device shows ("ZFP").
    pub fn short_fingerprint(&self) -> String {
        short_fingerprint(&self.seed_fingerprint)
    }
}

/// Progress of [`Wallet::init`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum InitEvent {
    /// About to ask lightwalletd for the tree state at `height`.
    FetchingTreeState { height: u32, server: String },
}

/// Progress of [`Wallet::sync`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SyncEvent {
    /// Connected; the server's chain tip.
    Started { server: String, tip: u64 },
    /// A range of blocks has been scanned.
    Scanned(ScannedRange),
}

/// Where the wallet stands after a sync.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SyncStatus {
    pub chain_tip: u32,
    pub fully_scanned: u32,
    /// Scan progress as numerator / denominator.
    pub progress: (u64, u64),
}

/// The lightwalletd server's view of the chain.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TipInfo {
    pub server: String,
    pub chain_name: String,
    pub height: u64,
    pub consensus_branch_id: String,
    pub vendor: String,
    pub version: String,
}

/// One pool's balance, in zatoshis.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PoolBalance {
    pub total: u64,
    pub spendable: u64,
    /// Change pending confirmation plus value pending spendability.
    pub pending: u64,
}

/// The account's balances, in zatoshis.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AccountBalances {
    pub orchard: PoolBalance,
    pub ironwood: PoolBalance,
    pub transparent: PoolBalance,
    pub total: u64,
    pub spendable: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Balances {
    pub chain_tip: u32,
    pub fully_scanned: u32,
    pub synced: bool,
    /// None when the account has no balance entry yet.
    pub account: Option<AccountBalances>,
}

/// A payment to make.
#[derive(Clone, Debug)]
pub struct SendRequest {
    /// Recipient: unified, Orchard-only unified, or transparent address.
    pub to: String,
    pub amount_zat: u64,
    /// Memo text (shielded recipients only).
    pub memo: Option<String>,
    /// Hand the device the compact signer view (v2 PCZT) instead of the
    /// full one.
    pub compact: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PaymentSummary {
    pub recipient: String,
    pub amount_zat: u64,
    /// The pool the payment goes to, e.g. "Orchard".
    pub pool: String,
    pub has_memo: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ChangeSummary {
    pub value_zat: u64,
    pub pool: String,
}

/// A built, unsigned send.
#[derive(Clone, Debug)]
pub struct SendProposal {
    pub target_height: u32,
    pub payments: Vec<PaymentSummary>,
    pub change: Vec<ChangeSummary>,
    pub fee_zat: u64,
    /// The PCZT to hand the signer (device or soft signer).
    pub signer_view: Vec<u8>,
    /// Whether `signer_view` is the compact view.
    pub compact: bool,
    /// SHA-256 of `signer_view`; the full PCZT is kept under this name.
    pub sha256: [u8; 32],
    /// Where the full PCZT was stored for proving.
    pub pending_path: PathBuf,
}

/// Progress of [`Wallet::finish`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum FinishEvent {
    /// The signatures were applied; each verified against its rk.
    SignaturesApplied { count: usize },
    Proving(ProveEvent),
    /// The transaction was extracted and stored in the wallet and in
    /// `raw_path`.
    Stored { txid: String, size: usize, raw_path: PathBuf },
}

/// A finished transaction.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Finished {
    pub txid: String,
    pub signatures: usize,
    /// The raw transaction, also written to `raw_path`.
    pub raw: Vec<u8>,
    pub raw_path: PathBuf,
    /// Whether lightwalletd accepted it.
    pub broadcast: bool,
}

/// The device's short seed fingerprint: the first four bytes, upper-case hex.
pub fn short_fingerprint(fp: &[u8; 32]) -> String {
    hex::encode_upper(&fp[..4])
}

/// Formats zatoshis as "1.23456789 ZEC (123456789 zat)".
pub fn format_zec(zat: u64) -> String {
    format!("{}.{:08} ZEC ({} zat)", zat / 100_000_000, zat % 100_000_000, zat)
}

/// Parses a ZEC amount with up to 8 decimals into zatoshis.
pub fn parse_zec(s: &str) -> Result<u64> {
    let (int, frac) = s.split_once('.').unwrap_or((s, ""));
    if frac.len() > 8 || int.is_empty() && frac.is_empty() {
        bail!("bad ZEC amount {s:?} (at most 8 decimals)");
    }
    let int: u64 = if int.is_empty() { 0 } else { int.parse().context("bad ZEC amount")? };
    let frac: u64 = if frac.is_empty() { 0 } else { format!("{frac:0<8}").parse().context("bad ZEC amount")? };
    int.checked_mul(100_000_000).and_then(|v| v.checked_add(frac)).ok_or_else(|| anyhow!("amount overflows"))
}

fn ua_request() -> UnifiedAddressRequest {
    // Orchard + P2PKH, the receivers the device can verify.
    UnifiedAddressRequest::custom(ReceiverRequirement::Require, ReceiverRequirement::Omit, ReceiverRequirement::Require)
        .expect("valid request")
}

/// Decodes a UFVK for `network` and checks it has the device account's
/// shape: Orchard, no Sapling.
pub fn parse_ufvk(network: Network, ufvk: &str) -> Result<UnifiedFullViewingKey> {
    let ufvk = UnifiedFullViewingKey::decode(&network, ufvk).map_err(|e| anyhow!("bad UFVK for this network: {e}"))?;
    if ufvk.sapling().is_some() {
        bail!("UFVK has a Sapling item; the LeekWallet account is Orchard + P2PKH only");
    }
    if ufvk.orchard().is_none() {
        bail!("UFVK has no Orchard item");
    }
    Ok(ufvk)
}

fn addresses_of(network: Network, ufvk: &UnifiedFullViewingKey) -> Result<Addresses> {
    let (ua, idx) = ufvk.default_address(ua_request()).map_err(|e| anyhow!("address derivation: {e:?}"))?;
    let (ua_o, _) = ufvk.default_address(UnifiedAddressRequest::ORCHARD).map_err(|e| anyhow!("{e:?}"))?;
    Ok(Addresses {
        unified: ua.encode(&network),
        diversifier_index: u128::from(idx),
        transparent: ua.transparent().map(|t| Address::Transparent(*t).encode(&network)),
        orchard_only: ua_o.encode(&network),
    })
}

/// The default addresses of a UFVK given as a string, without a wallet;
/// e.g. to check the address a device reports against its own UFVK.
pub fn addresses_for_ufvk(network: Network, ufvk: &str) -> Result<Addresses> {
    let ufvk = UnifiedFullViewingKey::decode(&network, ufvk).map_err(|e| anyhow!("bad UFVK for this network: {e}"))?;
    addresses_of(network, &ufvk)
}

/// The lightwalletd server's chain tip and identity.
pub async fn tip(server: &str) -> Result<TipInfo> {
    let mut client = lwd::connect(server).await?;
    let info = client.get_lightd_info(Empty {}).await.context("GetLightdInfo")?.into_inner();
    let tip = client.get_latest_block(ChainSpec {}).await.context("GetLatestBlock")?.into_inner();
    Ok(TipInfo {
        server: server.to_string(),
        chain_name: info.chain_name,
        height: tip.height,
        consensus_branch_id: info.consensus_branch_id,
        vendor: info.vendor,
        version: info.version,
    })
}

/// Sends a raw transaction through lightwalletd.
pub async fn broadcast(server: &str, raw: &[u8]) -> Result<()> {
    let mut client = lwd::connect(server).await?;
    let resp = client
        .send_transaction(RawTransaction { data: raw.to_vec(), height: 0 })
        .await
        .context("SendTransaction")?
        .into_inner();
    if resp.error_code != 0 {
        bail!("lightwalletd rejected the transaction: code {} {}", resp.error_code, resp.error_message);
    }
    Ok(())
}

/// An open wallet.
pub struct Wallet {
    profile: Profile,
    db: Db,
}

impl Wallet {
    /// Opens the wallet in `profile.data_dir`, which must exist and belong
    /// to `profile.network`.
    pub fn open(profile: Profile) -> Result<Self> {
        let db = open_db(&profile, false)?;
        Ok(Wallet { profile, db })
    }

    /// Opens the wallet, creating the data directory and database if needed.
    pub fn create(profile: Profile) -> Result<Self> {
        let db = open_db(&profile, true)?;
        Ok(Wallet { profile, db })
    }

    pub fn profile(&self) -> &Profile {
        &self.profile
    }

    /// Whether the wallet tracks an account yet.
    pub fn has_account(&self) -> Result<bool> {
        Ok(!self.db.get_account_ids()?.is_empty())
    }

    /// Creates the wallet in `profile` and imports `spec` as a watch-only
    /// account born at `birthday`, fetching the tree state before it from
    /// lightwalletd. Importing the same UFVK again is a no-op.
    pub async fn init(
        profile: Profile,
        spec: &AccountSpec,
        birthday: u32,
        mut on_event: impl FnMut(InitEvent),
    ) -> Result<(Self, InitOutcome)> {
        let ufvk = parse_ufvk(profile.network, &spec.ufvk)?;
        let mut w = Wallet::create(profile)?;
        if let Some(outcome) = w.already_imported(&ufvk)? {
            return Ok((w, outcome));
        }
        let height = birthday.saturating_sub(1);
        on_event(InitEvent::FetchingTreeState { height, server: w.profile.server.clone() });
        let mut client = lwd::connect(&w.profile.server).await?;
        let ts = client
            .get_tree_state(BlockId { height: u64::from(height), hash: vec![] })
            .await
            .context("GetTreeState")?
            .into_inner();
        let birthday = AccountBirthday::from_treestate(ts, None).map_err(|e| anyhow!("bad tree state: {e:?}"))?;
        let outcome = w.import(&ufvk, spec, &birthday)?;
        Ok((w, outcome))
    }

    /// [`Wallet::init`] with a birthday already in hand (no network).
    pub fn init_with_birthday(profile: Profile, spec: &AccountSpec, birthday: &AccountBirthday) -> Result<(Self, InitOutcome)> {
        let ufvk = parse_ufvk(profile.network, &spec.ufvk)?;
        let mut w = Wallet::create(profile)?;
        if let Some(outcome) = w.already_imported(&ufvk)? {
            return Ok((w, outcome));
        }
        let outcome = w.import(&ufvk, spec, birthday)?;
        Ok((w, outcome))
    }

    fn already_imported(&self, ufvk: &UnifiedFullViewingKey) -> Result<Option<InitOutcome>> {
        if self.db.get_account_ids()?.is_empty() {
            return Ok(None);
        }
        let (_, existing) = self.the_account()?;
        let net = &self.profile.network;
        if existing.encode(net)? == ufvk.encode(net)? {
            return Ok(Some(InitOutcome::AlreadyImported { addresses: addresses_of(*net, &existing)? }));
        }
        bail!("{} already tracks a different account", self.profile.data_dir.display());
    }

    fn import(&mut self, ufvk: &UnifiedFullViewingKey, spec: &AccountSpec, birthday: &AccountBirthday) -> Result<InitOutcome> {
        let account_index = zip32::AccountId::try_from(spec.account_index).map_err(|_| anyhow!("bad account index"))?;
        let fp = SeedFingerprint::from_bytes(spec.seed_fingerprint);
        // Recording the ZIP 32 derivation makes create_pczt_from_proposal put
        // zip32_derivation on our spends; the device uses it to find its key.
        let purpose = AccountPurpose::Spending { derivation: Some(Zip32Derivation::new(fp, account_index)) };
        let acct = self.db.import_account_ufvk(&spec.name, ufvk, birthday, purpose, Some("leekwallet"))?;
        Ok(InitOutcome::Imported(ImportedAccount {
            name: spec.name.clone(),
            uuid: acct.id().expose_uuid().to_string(),
            birthday: u32::from(birthday.height()),
            seed_fingerprint: spec.seed_fingerprint,
            account_index: spec.account_index,
            addresses: addresses_of(self.profile.network, ufvk)?,
        }))
    }

    /// The one account this companion tracks.
    fn the_account(&self) -> Result<(AccountUuid, UnifiedFullViewingKey)> {
        let ids = self.db.get_account_ids()?;
        let id = match ids.as_slice() {
            [id] => *id,
            [] => bail!("the wallet has no account; run `zecz init`"),
            _ => bail!("the wallet has {} accounts; zecz expects exactly one", ids.len()),
        };
        let acct = self.db.get_account(id)?.ok_or_else(|| anyhow!("account vanished"))?;
        let ufvk = acct.ufvk().cloned().ok_or_else(|| anyhow!("account has no UFVK"))?;
        Ok((id, ufvk))
    }

    /// The account's UFVK, encoded.
    pub fn ufvk(&self) -> Result<String> {
        Ok(self.the_account()?.1.encode(&self.profile.network)?)
    }

    /// The account's default addresses.
    pub fn addresses(&self) -> Result<Addresses> {
        let (_, ufvk) = self.the_account()?;
        addresses_of(self.profile.network, &ufvk)
    }

    /// Scans the chain through lightwalletd until the wallet is up to date,
    /// `batch` blocks at a time.
    pub async fn sync(&mut self, batch: u32, on_event: impl Fn(SyncEvent) + Send + Sync) -> Result<Option<SyncStatus>> {
        self.the_account()?;
        let mut client = lwd::connect(&self.profile.server).await?;
        let tip = client.get_latest_block(ChainSpec {}).await.context("GetLatestBlock")?.into_inner();
        on_event(SyncEvent::Started { server: self.profile.server.clone(), tip: tip.height });
        let cache = MemBlockCache::with_progress(|r| on_event(SyncEvent::Scanned(r)));
        *cache.tip.lock().unwrap() = Some(tip.height as u32);
        zcash_client_backend::sync::run(&mut client, &self.profile.network, &cache, &mut self.db, batch)
            .await
            .map_err(|e| anyhow!("sync failed: {e}"))?;
        self.sync_status()
    }

    /// Where the wallet stands, from its database; None before any sync.
    pub fn sync_status(&self) -> Result<Option<SyncStatus>> {
        Ok(self.db.get_wallet_summary(ConfirmationsPolicy::default())?.map(|s| SyncStatus {
            chain_tip: u32::from(s.chain_tip_height()),
            fully_scanned: u32::from(s.fully_scanned_height()),
            progress: (s.progress().scan().numerator().to_owned(), s.progress().scan().denominator().to_owned()),
        }))
    }

    /// The account's balances per pool.
    pub fn balances(&self) -> Result<Balances> {
        let (id, _) = self.the_account()?;
        let Some(s) = self.db.get_wallet_summary(ConfirmationsPolicy::default())? else {
            bail!("no balance yet; run `zecz sync`");
        };
        let pool = |b: &zcash_client_backend::data_api::Balance| PoolBalance {
            total: u64::from(b.total()),
            spendable: u64::from(b.spendable_value()),
            pending: u64::from(b.change_pending_confirmation()) + u64::from(b.value_pending_spendability()),
        };
        let account = s.account_balances().get(&id).map(|b| AccountBalances {
            orchard: pool(b.orchard_balance()),
            ironwood: pool(b.ironwood_balance()),
            transparent: pool(&b.unshielded_balance()),
            total: u64::from(b.total()),
            spendable: u64::from(b.spendable_value()),
        });
        Ok(Balances {
            chain_tip: u32::from(s.chain_tip_height()),
            fully_scanned: u32::from(s.fully_scanned_height()),
            synced: s.is_synced(),
            account,
        })
    }

    /// Proposes a payment and builds its PCZT. The full PCZT is stored in
    /// `pending/` for [`Wallet::finish`]; the signer view to hand the device
    /// is returned.
    pub fn propose_send(&mut self, req: &SendRequest) -> Result<SendProposal> {
        let params = self.profile.network;
        let (account_id, _) = self.the_account()?;

        let to_addr = Address::decode(&params, &req.to)
            .ok_or_else(|| anyhow!("{} is not a valid {} address", req.to, self.profile.net_name()))?;
        if matches!(to_addr, Address::Sapling(_)) {
            bail!("Sapling recipients are not supported");
        }
        let value = Zatoshis::from_u64(req.amount_zat).map_err(|_| anyhow!("amount out of range"))?;
        if value == Zatoshis::ZERO {
            bail!("amount must be positive");
        }
        let memo = req
            .memo
            .as_deref()
            .map(|m| -> Result<MemoBytes> { Ok(MemoBytes::from(Memo::from_bytes(m.as_bytes()).map_err(|e| anyhow!("memo: {e:?}"))?)) })
            .transpose()?;
        let payment = Payment::new(to_addr.to_zcash_address(&params), Some(value), memo, None, None, vec![])
            .map_err(|e| anyhow!("payment: {e:?}"))?;
        let request = TransactionRequest::new(vec![payment]).map_err(|e| anyhow!("request: {e:?}"))?;

        // Change goes to the Orchard-protocol pool current at the target height:
        // Ironwood once NU6.3 is active, Orchard before.
        let (target, _) = self
            .db
            .get_target_and_anchor_heights(ConfirmationsPolicy::default().trusted())?
            .ok_or_else(|| anyhow!("wallet is not synced; run `zecz sync`"))?;
        let target_h = BlockHeight::from(target);
        let change_pool = if params.is_nu_active(NetworkUpgrade::Nu6_3, target_h) {
            ShieldedPool::Ironwood
        } else {
            ShieldedPool::Orchard
        };

        let input_selector = GreedyInputSelector::<Db>::new();
        let change_strategy =
            SingleOutputChangeStrategy::<Db>::new(StandardFeeRule::Zip317, None, change_pool, DustOutputPolicy::default());
        let proposal = propose_transfer::<_, _, _, _, Infallible>(
            &mut self.db,
            &params,
            account_id,
            &input_selector,
            &change_strategy,
            request,
            ConfirmationsPolicy::default(),
            &SpendPolicy::default(),
            None,
            None,
        )
        .map_err(|e| anyhow!("cannot propose the transfer: {e}"))?;

        let step = proposal.steps().first();
        if !step.transparent_inputs().is_empty() {
            bail!("the proposal spends transparent inputs; the device does not sign those yet");
        }
        if step.input_in_pool(PoolType::SAPLING) {
            bail!("the proposal spends Sapling notes; not supported");
        }

        let mut rng = UnwrapErr(SysRng);
        let pczt = create_pczt_from_proposal::<_, _, Infallible, _, Infallible, _>(
            &mut self.db,
            &params,
            &SystemClock,
            &mut rng,
            account_id,
            OvkPolicy::Sender,
            &proposal,
            None,
            BundlePadding::DEFAULT,
        )
        .map_err(|e| anyhow!("cannot create the PCZT: {e}"))?;

        if !pczt.sapling().spends().is_empty() {
            bail!("PCZT has Sapling spends; refusing");
        }

        let view = redact_pczt_for_signer(&pczt, if req.compact { SignerView::Compact } else { SignerView::Full });
        let view_bytes = view.serialize().map_err(|e| anyhow!("PCZT encoding: {e:?}"))?;
        let full_bytes = pczt.serialize().map_err(|e| anyhow!("PCZT encoding: {e:?}"))?;
        let hash = zsig::sha256(&view_bytes);

        fs::create_dir_all(self.profile.pending_dir())?;
        let pending = self.pending_path(&hash);
        fs::write(&pending, &full_bytes)?;

        let payments = step
            .transaction_request()
            .payments()
            .iter()
            .map(|(i, p)| PaymentSummary {
                recipient: p.recipient_address().encode(),
                amount_zat: u64::from(p.amount().unwrap_or(Zatoshis::ZERO)),
                pool: step.payment_pools().get(i).map(|p| format!("{p}")).unwrap_or_default(),
                has_memo: p.memo().is_some(),
            })
            .collect();
        let change = step
            .balance()
            .proposed_change()
            .iter()
            .map(|c| ChangeSummary { value_zat: u64::from(c.value()), pool: format!("{}", c.output_pool()) })
            .collect();
        Ok(SendProposal {
            target_height: u32::from(target_h),
            payments,
            change,
            fee_zat: u64::from(step.balance().fee_required()),
            signer_view: view_bytes,
            compact: req.compact,
            sha256: hash,
            pending_path: pending,
        })
    }

    fn pending_path(&self, view_sha256: &[u8; 32]) -> PathBuf {
        self.profile.pending_dir().join(format!("{}.pczt", hex::encode(view_sha256)))
    }

    /// Finishes a send: checks `zsig` against the signer view `unsigned`
    /// that was handed to the signer, applies the signatures to the stored
    /// full PCZT, proves, extracts and stores the transaction, and (if
    /// `broadcast`) sends it through lightwalletd.
    pub async fn finish(
        &mut self,
        unsigned: &[u8],
        zsig: &[u8],
        broadcast: bool,
        mut on_event: impl FnMut(FinishEvent),
    ) -> Result<Finished> {
        let hash = zsig::sha256(unsigned);
        let z = Zsig::parse_for(zsig, unsigned)?;
        if z.entries.is_empty() {
            bail!("the .zsig carries no signatures");
        }

        let pending = self.pending_path(&hash);
        let full_bytes = fs::read(&pending).with_context(|| {
            format!("no pending PCZT {} ; was the unsigned PCZT created by `zecz send` with this data dir?", pending.display())
        })?;
        let full = Pczt::parse(&full_bytes).map_err(|e| anyhow!("pending PCZT: {e:?}"))?;
        let view = Pczt::parse(unsigned).map_err(|e| anyhow!("unsigned PCZT: {e:?}"))?;
        if !full.sapling().spends().is_empty() {
            bail!("PCZT has Sapling spends; not supported");
        }
        let sighash = Signer::new(full.clone()).map_err(|e| anyhow!("{e:?}"))?.shielded_sighash();
        if sighash != Signer::new(view).map_err(|e| anyhow!("{e:?}"))?.shielded_sighash() {
            bail!("the unsigned PCZT does not match the pending one (sighash differs)");
        }

        // Signatures first: cheap, and each is verified against its rk.
        let signed = apply_zsig(full, &z)?;
        on_event(FinishEvent::SignaturesApplied { count: z.entries.len() });

        let proven = prove_with(signed, |e| on_event(FinishEvent::Proving(e)))?;

        let mut rng = UnwrapErr(SysRng);
        let txid =
            extract_and_store_transaction_from_pczt::<_, ReceivedNoteId>(&mut self.db, &SystemClock, &mut rng, proven, None, None)
                .map_err(|e| anyhow!("transaction extraction failed: {e}"))?;
        let tx = self.db.get_transaction(txid)?.ok_or_else(|| anyhow!("stored transaction not found"))?;
        let mut raw = Vec::new();
        tx.write(&mut raw)?;
        let sent_dir = self.profile.sent_dir();
        fs::create_dir_all(&sent_dir)?;
        let raw_path = sent_dir.join(format!("{txid}.tx"));
        fs::write(&raw_path, &raw)?;
        let txid = txid.to_string();
        on_event(FinishEvent::Stored { txid: txid.clone(), size: raw.len(), raw_path: raw_path.clone() });

        if broadcast {
            self::broadcast(&self.profile.server, &raw).await?;
        }
        Ok(Finished { txid, signatures: z.entries.len(), raw, raw_path, broadcast })
    }

    /// [`Wallet::finish`] with the signer view and `.zsig` read from files.
    pub async fn finish_files(
        &mut self,
        unsigned: &Path,
        zsig: &Path,
        broadcast: bool,
        on_event: impl FnMut(FinishEvent),
    ) -> Result<Finished> {
        let unsigned = fs::read(unsigned).with_context(|| format!("reading {}", unsigned.display()))?;
        let zsig = fs::read(zsig).with_context(|| format!("reading {}", zsig.display()))?;
        self.finish(&unsigned, &zsig, broadcast, on_event).await
    }
}

/// Opens (and migrates) the wallet database. The data directory is bound
/// to one network, recorded in `network` beside the database.
fn open_db(profile: &Profile, create: bool) -> Result<Db> {
    let marker = profile.data_dir.join("network");
    let net = profile.net_name();
    if create {
        fs::create_dir_all(&profile.data_dir)?;
        match fs::read_to_string(&marker) {
            Ok(n) if n.trim() != net => {
                bail!("{} holds a {} wallet; pass --network {} or another --data-dir", profile.data_dir.display(), n.trim(), n.trim())
            }
            Ok(_) => {}
            Err(_) => fs::write(&marker, net)?,
        }
    } else {
        let n = fs::read_to_string(&marker)
            .with_context(|| format!("no wallet in {}; run `zecz init` first", profile.data_dir.display()))?;
        if n.trim() != net {
            bail!("{} holds a {} wallet; pass --network {}", profile.data_dir.display(), n.trim(), n.trim());
        }
    }
    let mut db = WalletDb::for_path(profile.db_path(), profile.network, SystemClock, UnwrapErr(SysRng))?;
    init_wallet_db(&mut db, None).map_err(|e| anyhow!("wallet database migration failed: {e}"))?;
    Ok(db)
}
