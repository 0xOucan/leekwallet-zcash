//! `zecz`: LeekWallet Zcash development companion.
//!
//! A watch-only wallet for a hardware-wallet account (Orchard + P2PKH UFVK,
//! no Sapling). It builds PCZTs for the device to sign, proves them while
//! the device signs, applies the returned `.zsig`, and broadcasts.

use std::convert::Infallible;
use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{anyhow, bail, Context, Result};
use clap::{Parser, Subcommand, ValueEnum};
use pczt::roles::signer::Signer;
use pczt::Pczt;
use rand::{rand_core::UnwrapErr, rngs::SysRng};
use zcash_client_backend::data_api::wallet::{
    create_pczt_from_proposal, extract_and_store_transaction_from_pczt, input_selection::GreedyInputSelector,
    input_selection::SpendPolicy, propose_transfer, redact_pczt_for_signer, ConfirmationsPolicy, SignerView,
};
use zcash_client_backend::data_api::{
    Account as _, AccountBirthday, AccountPurpose, WalletRead, WalletWrite, Zip32Derivation,
};
use zcash_client_backend::fees::{standard::SingleOutputChangeStrategy, DustOutputPolicy, StandardFeeRule};
use zcash_client_backend::proto::service::{BlockId, ChainSpec, RawTransaction};
use zcash_client_backend::util::SystemClock;
use zcash_client_backend::wallet::OvkPolicy;
use zcash_client_sqlite::{wallet::init::init_wallet_db, AccountUuid, ReceivedNoteId, WalletDb};
use zcash_keys::address::Address;
use zcash_keys::keys::{ReceiverRequirement, UnifiedAddressRequest, UnifiedFullViewingKey};
use zcash_primitives::transaction::builder::BundlePadding;
use zcash_protocol::consensus::{BlockHeight, Network, NetworkConstants, NetworkType, NetworkUpgrade, Parameters};
use zcash_protocol::memo::{Memo, MemoBytes};
use zcash_protocol::value::Zatoshis;
use zcash_protocol::{PoolType, ShieldedPool};
use zip321::{Payment, TransactionRequest};
use zip32::fingerprint::SeedFingerprint;

use zecz::authorize::{apply_zsig, prove};
use zecz::device::{self, Device};
use zecz::keys::DevAccount;
use zecz::lwd::{self, MemBlockCache};
use zecz::soft_signer;
use zecz::zsig::{self, Zsig};

type Db = WalletDb<rusqlite::Connection, Network, SystemClock, UnwrapErr<SysRng>>;

#[derive(Clone, Copy, Debug, ValueEnum)]
enum Net {
    Test,
    Main,
}

#[derive(Parser)]
#[command(name = "zecz", version, about = "LeekWallet Zcash development companion (watch-only wallet, PCZT builder, prover)")]
struct Cli {
    /// Zcash network.
    #[arg(long, global = true, value_enum, default_value = "test")]
    network: Net,
    /// Wallet data directory.
    #[arg(long, global = true, default_value = "./zecz-data")]
    data_dir: PathBuf,
    /// lightwalletd gRPC endpoint [default: https://testnet.zec.rocks:443 (test), https://zec.rocks:443 (main)]
    #[arg(long, global = true)]
    server: Option<String>,
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Create the wallet and import the device account (UFVK) as watch-only.
    Init {
        /// Unified full viewing key exported by the device (Orchard + P2PKH).
        #[arg(long)]
        ufvk: String,
        /// Account birthday height (first block that may contain its notes).
        #[arg(long)]
        birthday: u32,
        /// ZIP 32 seed fingerprint (32 bytes hex) of the device seed.
        #[arg(long)]
        seed_fingerprint: String,
        /// ZIP 32 account index.
        #[arg(long)]
        account_index: u32,
        #[arg(long, default_value = "leekwallet")]
        name: String,
    },
    /// DEV ONLY: derive the device account from a mnemonic and run `init`.
    InitDev {
        #[arg(long)]
        mnemonic: String,
        #[arg(long, default_value = "")]
        passphrase: String,
        #[arg(long, default_value_t = 0)]
        account: u32,
        #[arg(long)]
        birthday: u32,
        #[arg(long, default_value = "leekwallet-dev")]
        name: String,
    },
    /// Scan the chain through lightwalletd until the wallet is up to date.
    Sync {
        /// Blocks per scan batch.
        #[arg(long, default_value_t = 1000)]
        batch: u32,
    },
    /// Show the lightwalletd server's chain tip (handy for picking a birthday).
    Tip,
    /// Show balances.
    Balance,
    /// Show the default unified address and its transparent address.
    Address,
    /// Build an unsigned PCZT for the device.
    Send {
        /// Recipient (unified, Orchard-only unified, or transparent address).
        #[arg(long)]
        to: String,
        /// Amount in zatoshis (or ZEC with --zec).
        #[arg(long)]
        amount: String,
        /// Interpret --amount as ZEC.
        #[arg(long)]
        zec: bool,
        /// Memo text (shielded recipients only).
        #[arg(long)]
        memo: Option<String>,
        /// Where to write the signer view for the device.
        #[arg(long)]
        out: PathBuf,
    },
    /// Pair with the device, export its account (UFVK) and run `init`.
    DeviceInit {
        /// Serial port of the device (e.g. /dev/ttyACM0), or a zdev PTY.
        #[arg(long)]
        port: String,
        /// ZIP 32 account index on the device.
        #[arg(long, default_value_t = 0)]
        account: u32,
        /// Account birthday height (first block that may contain its notes).
        #[arg(long)]
        birthday: u32,
        #[arg(long, default_value = "leekwallet")]
        name: String,
    },
    /// Have the device check, show and sign a PCZT; write its .zsig.
    SignDevice {
        #[arg(long)]
        port: String,
        /// ZIP 32 account index on the device.
        #[arg(long, default_value_t = 0)]
        account: u32,
        /// The unsigned PCZT (from `send --out`).
        #[arg(long = "in")]
        input: PathBuf,
        #[arg(long)]
        out: PathBuf,
    },
    /// DEV ONLY: sign a PCZT with keys from a mnemonic and write a .zsig.
    SignSoft {
        #[arg(long)]
        mnemonic: String,
        #[arg(long, default_value = "")]
        passphrase: String,
        #[arg(long, default_value_t = 0)]
        account: u32,
        #[arg(long = "in")]
        input: PathBuf,
        #[arg(long)]
        out: PathBuf,
    },
    /// Prove, apply the device signatures, store, and broadcast.
    Finish {
        /// The unsigned PCZT that was given to the signer (from `send --out`).
        #[arg(long)]
        pczt: PathBuf,
        /// The signer's .zsig.
        #[arg(long)]
        sigs: PathBuf,
        /// Build and store the transaction, but do not broadcast it.
        #[arg(long)]
        no_broadcast: bool,
    },
}

struct Ctx {
    params: Network,
    data_dir: PathBuf,
    server: String,
}

impl Ctx {
    fn db_path(&self) -> PathBuf {
        self.data_dir.join("wallet.sqlite")
    }

    fn pending_dir(&self) -> PathBuf {
        self.data_dir.join("pending")
    }

    fn net_name(&self) -> &'static str {
        match self.params.network_type() {
            NetworkType::Test => "test",
            NetworkType::Main => "main",
            NetworkType::Regtest => "regtest",
        }
    }

    /// Opens (and migrates) the wallet database. The data directory is bound
    /// to one network, recorded in `network` beside the database.
    fn open_db(&self, create: bool) -> Result<Db> {
        let marker = self.data_dir.join("network");
        if create {
            fs::create_dir_all(&self.data_dir)?;
            match fs::read_to_string(&marker) {
                Ok(n) if n.trim() != self.net_name() => {
                    bail!("{} holds a {} wallet; pass --network {} or another --data-dir", self.data_dir.display(), n.trim(), n.trim())
                }
                Ok(_) => {}
                Err(_) => fs::write(&marker, self.net_name())?,
            }
        } else {
            let n = fs::read_to_string(&marker)
                .with_context(|| format!("no wallet in {}; run `zecz init` first", self.data_dir.display()))?;
            if n.trim() != self.net_name() {
                bail!("{} holds a {} wallet; pass --network {}", self.data_dir.display(), n.trim(), n.trim());
            }
        }
        let mut db = WalletDb::for_path(self.db_path(), self.params, SystemClock, UnwrapErr(SysRng))?;
        init_wallet_db(&mut db, None).map_err(|e| anyhow!("wallet database migration failed: {e}"))?;
        Ok(db)
    }
}

/// The one account this companion tracks.
fn the_account(db: &Db) -> Result<(AccountUuid, UnifiedFullViewingKey)> {
    let ids = db.get_account_ids()?;
    let id = match ids.as_slice() {
        [id] => *id,
        [] => bail!("the wallet has no account; run `zecz init`"),
        _ => bail!("the wallet has {} accounts; zecz expects exactly one", ids.len()),
    };
    let acct = db.get_account(id)?.ok_or_else(|| anyhow!("account vanished"))?;
    let ufvk = acct.ufvk().cloned().ok_or_else(|| anyhow!("account has no UFVK"))?;
    Ok((id, ufvk))
}

fn ua_request() -> UnifiedAddressRequest {
    // Orchard + P2PKH, the receivers the device can verify.
    UnifiedAddressRequest::custom(ReceiverRequirement::Require, ReceiverRequirement::Omit, ReceiverRequirement::Require)
        .expect("valid request")
}

fn fmt_zec(z: Zatoshis) -> String {
    let v = u64::from(z);
    format!("{}.{:08} ZEC ({} zat)", v / 100_000_000, v % 100_000_000, v)
}

fn parse_zec(s: &str) -> Result<u64> {
    let (int, frac) = s.split_once('.').unwrap_or((s, ""));
    if frac.len() > 8 || int.is_empty() && frac.is_empty() {
        bail!("bad ZEC amount {s:?} (at most 8 decimals)");
    }
    let int: u64 = if int.is_empty() { 0 } else { int.parse().context("bad ZEC amount")? };
    let frac: u64 = if frac.is_empty() { 0 } else { format!("{frac:0<8}").parse().context("bad ZEC amount")? };
    int.checked_mul(100_000_000).and_then(|v| v.checked_add(frac)).ok_or_else(|| anyhow!("amount overflows"))
}

fn print_addresses(params: &Network, ufvk: &UnifiedFullViewingKey) -> Result<()> {
    let (ua, idx) = ufvk.default_address(ua_request()).map_err(|e| anyhow!("address derivation: {e:?}"))?;
    println!("Unified address (Orchard + P2PKH, diversifier {}):", u128::from(idx));
    println!("  {}", ua.encode(params));
    if let Some(t) = ua.transparent() {
        println!("Transparent address (same index):");
        println!("  {}", Address::Transparent(*t).encode(params));
    }
    let (ua_o, _) = ufvk.default_address(UnifiedAddressRequest::ORCHARD).map_err(|e| anyhow!("{e:?}"))?;
    println!("Orchard-only unified address:");
    println!("  {}", ua_o.encode(params));
    Ok(())
}

async fn cmd_init(
    ctx: &Ctx,
    ufvk_s: &str,
    birthday: u32,
    fp: SeedFingerprint,
    account_index: u32,
    name: &str,
) -> Result<()> {
    let ufvk = UnifiedFullViewingKey::decode(&ctx.params, ufvk_s).map_err(|e| anyhow!("bad UFVK for this network: {e}"))?;
    if ufvk.sapling().is_some() {
        bail!("UFVK has a Sapling item; the LeekWallet account is Orchard + P2PKH only");
    }
    if ufvk.orchard().is_none() {
        bail!("UFVK has no Orchard item");
    }
    let account_index = zip32::AccountId::try_from(account_index).map_err(|_| anyhow!("bad account index"))?;

    let mut db = ctx.open_db(true)?;
    if !db.get_account_ids()?.is_empty() {
        let (_, existing) = the_account(&db)?;
        if existing.encode(&ctx.params)? == ufvk.encode(&ctx.params)? {
            println!("Account already imported in {}.", ctx.data_dir.display());
            return print_addresses(&ctx.params, &existing);
        }
        bail!("{} already tracks a different account", ctx.data_dir.display());
    }

    eprintln!("Fetching tree state at height {} from {} ...", birthday.saturating_sub(1), ctx.server);
    let mut client = lwd::connect(&ctx.server).await?;
    let ts = client
        .get_tree_state(BlockId { height: u64::from(birthday.saturating_sub(1)), hash: vec![] })
        .await
        .context("GetTreeState")?
        .into_inner();
    let birthday = AccountBirthday::from_treestate(ts, None).map_err(|e| anyhow!("bad tree state: {e:?}"))?;

    // Recording the ZIP 32 derivation makes create_pczt_from_proposal put
    // zip32_derivation on our spends; the device uses it to find its key.
    let purpose = AccountPurpose::Spending { derivation: Some(Zip32Derivation::new(fp, account_index)) };
    let acct = db.import_account_ufvk(name, &ufvk, &birthday, purpose, Some("leekwallet"))?;
    println!("Imported account {:?} ({}), birthday {}", name, acct.id().expose_uuid(), u32::from(birthday.height()));
    println!("Seed fingerprint {}, ZIP 32 account {}", hex::encode(fp.to_bytes()), u32::from(account_index));
    // The short form the device shows on its home and wallet screens.
    println!("ZFP {} (compare with the device)", short_fp(&fp));
    print_addresses(&ctx.params, &ufvk)
}

async fn cmd_sync(ctx: &Ctx, batch: u32) -> Result<()> {
    let mut db = ctx.open_db(false)?;
    the_account(&db)?;
    let mut client = lwd::connect(&ctx.server).await?;
    let tip = client.get_latest_block(ChainSpec {}).await.context("GetLatestBlock")?.into_inner();
    eprintln!("Syncing with {} (chain tip {}) ...", ctx.server, tip.height);
    let cache = MemBlockCache::default();
    *cache.tip.lock().unwrap() = Some(tip.height as u32);
    zcash_client_backend::sync::run(&mut client, &ctx.params, &cache, &mut db, batch)
        .await
        .map_err(|e| anyhow!("sync failed: {e}"))?;
    let summary = db.get_wallet_summary(ConfirmationsPolicy::default())?;
    match summary {
        Some(s) => println!(
            "Synced: chain tip {}, fully scanned to {}, scan progress {}/{}",
            u32::from(s.chain_tip_height()),
            u32::from(s.fully_scanned_height()),
            s.progress().scan().numerator(),
            s.progress().scan().denominator()
        ),
        None => println!("Synced, but the wallet has no summary yet"),
    }
    Ok(())
}

async fn cmd_tip(ctx: &Ctx) -> Result<()> {
    let mut client = lwd::connect(&ctx.server).await?;
    let info = client.get_lightd_info(zcash_client_backend::proto::service::Empty {}).await.context("GetLightdInfo")?.into_inner();
    let tip = client.get_latest_block(ChainSpec {}).await.context("GetLatestBlock")?.into_inner();
    println!("{}: chain {:?}, tip {} ({}), server {} {}", ctx.server, info.chain_name, tip.height, info.consensus_branch_id, info.vendor, info.version);
    Ok(())
}

fn cmd_balance(ctx: &Ctx) -> Result<()> {
    let db = ctx.open_db(false)?;
    let (id, _) = the_account(&db)?;
    let Some(s) = db.get_wallet_summary(ConfirmationsPolicy::default())? else {
        bail!("no balance yet; run `zecz sync`");
    };
    println!("Chain tip {}, fully scanned to {}", u32::from(s.chain_tip_height()), u32::from(s.fully_scanned_height()));
    if !s.is_synced() {
        println!("(not fully synced; balances may be incomplete)");
    }
    let Some(b) = s.account_balances().get(&id) else {
        println!("Total: {}", fmt_zec(Zatoshis::ZERO));
        return Ok(());
    };
    let pool = |label: &str, bal: &zcash_client_backend::data_api::Balance| {
        println!(
            "  {label:<12} {}  (spendable {}, pending {})",
            fmt_zec(bal.total()),
            u64::from(bal.spendable_value()),
            u64::from(bal.change_pending_confirmation()) + u64::from(bal.value_pending_spendability())
        );
    };
    pool("Orchard", b.orchard_balance());
    pool("Ironwood", b.ironwood_balance());
    pool("Transparent", &b.unshielded_balance());
    println!("Total:     {}", fmt_zec(b.total()));
    println!("Spendable: {}", fmt_zec(b.spendable_value()));
    if b.unshielded_balance().total() > Zatoshis::ZERO {
        println!("Note: the device does not sign transparent inputs yet; transparent funds cannot be sent from here.");
    }
    Ok(())
}

fn cmd_address(ctx: &Ctx) -> Result<()> {
    let db = ctx.open_db(false)?;
    let (_, ufvk) = the_account(&db)?;
    print_addresses(&ctx.params, &ufvk)
}

fn cmd_send(ctx: &Ctx, to: &str, amount: &str, zec: bool, memo: Option<&str>, out: &Path) -> Result<()> {
    let mut db = ctx.open_db(false)?;
    let (account_id, _) = the_account(&db)?;

    let to_addr = Address::decode(&ctx.params, to).ok_or_else(|| anyhow!("{to} is not a valid {} address", ctx.net_name()))?;
    if matches!(to_addr, Address::Sapling(_)) {
        bail!("Sapling recipients are not supported");
    }
    let zats = if zec { parse_zec(amount)? } else { amount.parse::<u64>().context("amount must be zatoshis (or pass --zec)")? };
    let value = Zatoshis::from_u64(zats).map_err(|_| anyhow!("amount out of range"))?;
    if value == Zatoshis::ZERO {
        bail!("amount must be positive");
    }
    let memo = memo
        .map(|m| -> Result<MemoBytes> { Ok(MemoBytes::from(Memo::from_bytes(m.as_bytes()).map_err(|e| anyhow!("memo: {e:?}"))?)) })
        .transpose()?;
    let payment = Payment::new(to_addr.to_zcash_address(&ctx.params), Some(value), memo, None, None, vec![])
        .map_err(|e| anyhow!("payment: {e:?}"))?;
    let request = TransactionRequest::new(vec![payment]).map_err(|e| anyhow!("request: {e:?}"))?;

    // Change goes to the Orchard-protocol pool current at the target height:
    // Ironwood once NU6.3 is active, Orchard before.
    let (target, _) = db
        .get_target_and_anchor_heights(ConfirmationsPolicy::default().trusted())?
        .ok_or_else(|| anyhow!("wallet is not synced; run `zecz sync`"))?;
    let target_h = BlockHeight::from(target);
    let change_pool = if ctx.params.is_nu_active(NetworkUpgrade::Nu6_3, target_h) {
        ShieldedPool::Ironwood
    } else {
        ShieldedPool::Orchard
    };

    let input_selector = GreedyInputSelector::<Db>::new();
    let change_strategy =
        SingleOutputChangeStrategy::<Db>::new(StandardFeeRule::Zip317, None, change_pool, DustOutputPolicy::default());
    let proposal = propose_transfer::<_, _, _, _, Infallible>(
        &mut db,
        &ctx.params,
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
        &mut db,
        &ctx.params,
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

    let view = redact_pczt_for_signer(&pczt, SignerView::Full);
    let view_bytes = view.serialize().map_err(|e| anyhow!("PCZT encoding: {e:?}"))?;
    let full_bytes = pczt.serialize().map_err(|e| anyhow!("PCZT encoding: {e:?}"))?;
    let hash = zsig::sha256(&view_bytes);

    fs::create_dir_all(ctx.pending_dir())?;
    let pending = ctx.pending_dir().join(format!("{}.pczt", hex::encode(hash)));
    fs::write(&pending, &full_bytes)?;
    fs::write(out, &view_bytes).with_context(|| format!("writing {}", out.display()))?;

    println!("Proposal (target height {}):", u32::from(target_h));
    for (i, p) in step.transaction_request().payments() {
        let pool = step.payment_pools().get(i).map(|p| format!("{p}")).unwrap_or_default();
        println!(
            "  pay  {} to {} [{pool}]{}",
            fmt_zec(p.amount().unwrap_or(Zatoshis::ZERO)),
            p.recipient_address().encode(),
            if p.memo().is_some() { " with memo" } else { "" }
        );
    }
    for c in step.balance().proposed_change() {
        println!("  change {} [{}]", fmt_zec(c.value()), c.output_pool());
    }
    println!("  fee  {}", fmt_zec(step.balance().fee_required()));
    println!("Unsigned PCZT (signer view, {} B) -> {}", view_bytes.len(), out.display());
    println!("  sha256 {}", hex::encode(hash));
    println!("Full PCZT kept for proving -> {}", pending.display());
    Ok(())
}

/// Open the device, pair (the user compares the passkey) and make sure it is
/// unlocked.
fn connect_device(port: &str) -> Result<Device> {
    let mut dev = Device::open(port)?;
    dev.pair(|passkey| {
        println!("Pairing passkey: {passkey}");
        println!("Check that the device shows the same six digits, then press ALLOW on it.");
    })?;
    println!("Paired.");
    dev.ensure_unlocked()?;
    Ok(dev)
}

/// The device's short seed fingerprint: the first four bytes, upper-case hex.
fn short_fp(fp: &SeedFingerprint) -> String {
    hex::encode_upper(&fp.to_bytes()[..4])
}

async fn cmd_device_init(ctx: &Ctx, port: &str, account: u32, birthday: u32, name: &str) -> Result<()> {
    let mut dev = connect_device(port)?;
    let a = dev.z_get_account(account, ctx.net_name())?;
    if device::get_text(&a, "network")? != ctx.net_name() {
        bail!("the device answered for a different network");
    }
    let ufvk = device::get_text(&a, "ufvk")?;
    let fp: [u8; 32] = device::get_bytes(&a, "seedFingerprint")?
        .try_into()
        .map_err(|_| anyhow!("seedFingerprint is not 32 bytes"))?;
    let device_ua = device::get_text(&a, "ua")?;
    println!("Device account {account}: {device_ua}");
    println!("Device t-address: {}", device::get_text(&a, "tAddress")?);

    // The device derives its address itself; the UFVK must lead to the same one.
    let parsed = UnifiedFullViewingKey::decode(&ctx.params, &ufvk).map_err(|e| anyhow!("device UFVK: {e}"))?;
    let (ua, _) = parsed.default_address(ua_request()).map_err(|e| anyhow!("address derivation: {e:?}"))?;
    if ua.encode(&ctx.params) != device_ua {
        bail!("the device's address does not match its own viewing key; refusing to import");
    }
    cmd_init(ctx, &ufvk, birthday, SeedFingerprint::from_bytes(fp), account, name).await
}

fn cmd_sign_device(ctx: &Ctx, port: &str, account: u32, input: &Path, out: &Path) -> Result<()> {
    let bytes = fs::read(input).with_context(|| format!("reading {}", input.display()))?;
    let mut dev = connect_device(port)?;
    println!("Uploading {} bytes (sha256 {}) ...", bytes.len(), hex::encode(zsig::sha256(&bytes)));
    println!("Review the transaction on the device; ACCEPT on its last page signs.");
    let (sighash, entries) = dev.z_sign(&bytes, account, ctx.net_name(), |done, total| {
        eprint!("\r  {done}/{total} bytes");
        if done == total {
            eprintln!();
        }
    })?;
    let z = Zsig::new(&bytes, entries);
    fs::write(out, z.to_bytes()?)?;
    println!("Device signed {} spend(s); sighash {}", z.entries.len(), hex::encode(sighash));
    for e in &z.entries {
        println!("  {:?} action {}", e.pool, e.index);
    }
    println!("Signatures -> {}", out.display());
    Ok(())
}

fn cmd_sign_soft(ctx: &Ctx, mnemonic: &str, passphrase: &str, account: u32, input: &Path, out: &Path) -> Result<()> {
    eprintln!("WARNING: sign-soft is a DEV ONLY reference signer. It does not check what it signs.");
    let bytes = fs::read(input).with_context(|| format!("reading {}", input.display()))?;
    let acct = DevAccount::from_mnemonic(&ctx.params, mnemonic, passphrase, account)?;
    let z = soft_signer::sign(&bytes, &acct, ctx.params.network_type().coin_type())?;
    fs::write(out, z.to_bytes()?)?;
    println!("Signed {} spend(s) of {} (sha256 {})", z.entries.len(), input.display(), hex::encode(z.pczt_sha256));
    for e in &z.entries {
        println!("  {:?} action {}", e.pool, e.index);
    }
    println!("Signatures -> {}", out.display());
    Ok(())
}

async fn cmd_finish(ctx: &Ctx, pczt_path: &Path, sigs_path: &Path, no_broadcast: bool) -> Result<()> {
    let unsigned = fs::read(pczt_path).with_context(|| format!("reading {}", pczt_path.display()))?;
    let hash = zsig::sha256(&unsigned);
    let zsig_bytes = fs::read(sigs_path).with_context(|| format!("reading {}", sigs_path.display()))?;
    let z = Zsig::parse_for(&zsig_bytes, &unsigned)?;
    if z.entries.is_empty() {
        bail!("the .zsig carries no signatures");
    }

    let pending = ctx.pending_dir().join(format!("{}.pczt", hex::encode(hash)));
    let full_bytes = fs::read(&pending)
        .with_context(|| format!("no pending PCZT {} ; was {} created by `zecz send` with this data dir?", pending.display(), pczt_path.display()))?;
    let full = Pczt::parse(&full_bytes).map_err(|e| anyhow!("pending PCZT: {e:?}"))?;
    let view = Pczt::parse(&unsigned).map_err(|e| anyhow!("unsigned PCZT: {e:?}"))?;
    if !full.sapling().spends().is_empty() {
        bail!("PCZT has Sapling spends; not supported");
    }
    let sighash = Signer::new(full.clone()).map_err(|e| anyhow!("{e:?}"))?.shielded_sighash();
    if sighash != Signer::new(view).map_err(|e| anyhow!("{e:?}"))?.shielded_sighash() {
        bail!("the unsigned PCZT does not match the pending one (sighash differs)");
    }

    // Signatures first: cheap, and each is verified against its rk.
    let signed = apply_zsig(full, &z)?;
    println!("Applied {} signature(s); all verify against rk", z.entries.len());

    let proven = prove(signed)?;

    let mut db = ctx.open_db(false)?;
    let mut rng = UnwrapErr(SysRng);
    let txid = extract_and_store_transaction_from_pczt::<_, ReceivedNoteId>(&mut db, &SystemClock, &mut rng, proven, None, None)
        .map_err(|e| anyhow!("transaction extraction failed: {e}"))?;
    let tx = db.get_transaction(txid)?.ok_or_else(|| anyhow!("stored transaction not found"))?;
    let mut raw = Vec::new();
    tx.write(&mut raw)?;
    let sent_dir = ctx.data_dir.join("sent");
    fs::create_dir_all(&sent_dir)?;
    fs::write(sent_dir.join(format!("{txid}.tx")), &raw)?;
    println!("Transaction {txid} ({} B) stored in the wallet", raw.len());

    if no_broadcast {
        println!("Not broadcast (--no-broadcast). Raw tx: {}", sent_dir.join(format!("{txid}.tx")).display());
        return Ok(());
    }
    let mut client = lwd::connect(&ctx.server).await?;
    let resp = client
        .send_transaction(RawTransaction { data: raw, height: 0 })
        .await
        .context("SendTransaction")?
        .into_inner();
    if resp.error_code != 0 {
        bail!("lightwalletd rejected the transaction: code {} {}", resp.error_code, resp.error_message);
    }
    println!("Broadcast OK: txid {txid}");
    Ok(())
}

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();
    let (params, default_server) = match cli.network {
        Net::Test => (Network::TestNetwork, "https://testnet.zec.rocks:443"),
        Net::Main => (Network::MainNetwork, "https://zec.rocks:443"),
    };
    let ctx = Ctx { params, data_dir: cli.data_dir, server: cli.server.unwrap_or_else(|| default_server.into()) };

    match cli.cmd {
        Cmd::Init { ufvk, birthday, seed_fingerprint, account_index, name } => {
            let fp: [u8; 32] = hex::decode(&seed_fingerprint)
                .ok()
                .and_then(|b| b.try_into().ok())
                .ok_or_else(|| anyhow!("--seed-fingerprint must be 32 bytes of hex"))?;
            cmd_init(&ctx, &ufvk, birthday, SeedFingerprint::from_bytes(fp), account_index, &name).await
        }
        Cmd::InitDev { mnemonic, passphrase, account, birthday, name } => {
            eprintln!("WARNING: init-dev is DEV ONLY. It handles a mnemonic on the host; never use it with real funds.");
            let acct = DevAccount::from_mnemonic(&ctx.params, &mnemonic, &passphrase, account)?;
            let ufvk = acct.ufvk.encode(&ctx.params)?;
            println!("UFVK: {ufvk}");
            cmd_init(&ctx, &ufvk, birthday, acct.seed_fingerprint, account, &name).await
        }
        Cmd::Sync { batch } => cmd_sync(&ctx, batch).await,
        Cmd::Tip => cmd_tip(&ctx).await,
        Cmd::Balance => cmd_balance(&ctx),
        Cmd::Address => cmd_address(&ctx),
        Cmd::Send { to, amount, zec, memo, out } => cmd_send(&ctx, &to, &amount, zec, memo.as_deref(), &out),
        Cmd::DeviceInit { port, account, birthday, name } => cmd_device_init(&ctx, &port, account, birthday, &name).await,
        Cmd::SignDevice { port, account, input, out } => cmd_sign_device(&ctx, &port, account, &input, &out),
        Cmd::SignSoft { mnemonic, passphrase, account, input, out } => {
            cmd_sign_soft(&ctx, &mnemonic, &passphrase, account, &input, &out)
        }
        Cmd::Finish { pczt, sigs, no_broadcast } => cmd_finish(&ctx, &pczt, &sigs, no_broadcast).await,
    }
}
