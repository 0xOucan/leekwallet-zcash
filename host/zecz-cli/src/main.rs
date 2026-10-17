//! `zecz`: LeekWallet Zcash development companion.
//!
//! A watch-only wallet for a hardware-wallet account (Orchard + P2PKH UFVK,
//! no Sapling). It builds PCZTs for the device to sign, proves them while
//! the device signs, applies the returned `.zsig`, and broadcasts.
//!
//! The work is done by the `zecz` library (zecz-core); this binary parses
//! arguments and prints what the library returns.

use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{anyhow, bail, Context, Result};
use clap::{Parser, Subcommand, ValueEnum};

use zecz::device::{self, Device};
use zecz::keys::DevAccount;
use zecz::seed_card::{self, NewCard};
use zecz::soft_signer;
use zecz::wallet::{
    self, format_zec, parse_zec, AccountSpec, Addresses, FinishEvent, InitEvent, InitOutcome, SendRequest, SyncEvent,
};
use zecz::zsig::{self, Zsig};
use zecz::{Network, Profile, Wallet};

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
    /// Seal a seed phrase into a .zkv file for the device's SD card.
    /// Handles the phrase on this computer: use an offline machine for real
    /// funds. The file passphrase is read from ZECZ_ZKV_PASSPHRASE or stdin.
    ZkvNew {
        #[arg(long)]
        mnemonic: String,
        /// Output file; the device lists 8.3 names such as SEED1.ZKV.
        #[arg(long)]
        out: PathBuf,
        #[arg(long)]
        birthday: u32,
        #[arg(long, default_value_t = 0)]
        account: u32,
        #[arg(long, default_value = "")]
        label: String,
        #[arg(long, default_value_t = zecz::zkv::ITERATIONS_DEFAULT)]
        iterations: u32,
    },
    /// Open a .zkv file and show what the device will load from it (seed
    /// fingerprint, account, birthday, UFVK). Never prints the phrase.
    ZkvOpen {
        #[arg(long = "in")]
        input: PathBuf,
        /// BIP 39 passphrase applied on top, as on the device.
        #[arg(long, default_value = "")]
        passphrase: String,
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
        /// Write the compact signer view (v2 PCZT, about half the bytes and
        /// QR frames). The device rebuilds the fields it leaves out.
        #[arg(long)]
        compact: bool,
    },
    /// Build an unsigned PCZT that moves every transparent coin of the
    /// account into its shielded pool (Ironwood from NU6.3).
    Shield {
        /// Where to write the signer view for the device.
        #[arg(long)]
        out: PathBuf,
        /// Write the compact signer view (v2 PCZT).
        #[arg(long)]
        compact: bool,
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
        /// Optional when the device already knows it; given, it is also
        /// recorded on the device for the next companion.
        #[arg(long)]
        birthday: Option<u32>,
        #[arg(long, default_value = "leekwallet")]
        name: String,
    },
    /// Show this wallet's address on the device; the user confirms there
    /// that it is the same one.
    DeviceShowAddress {
        #[arg(long)]
        port: String,
        #[arg(long, default_value_t = 0)]
        account: u32,
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

fn print_addresses(a: &Addresses) {
    println!("Unified address (Orchard + P2PKH, diversifier {}):", a.diversifier_index);
    println!("  {}", a.unified);
    if let Some(t) = &a.transparent {
        println!("Transparent address (same index):");
        println!("  {t}");
    }
    println!("Orchard-only unified address:");
    println!("  {}", a.orchard_only);
}

fn print_progress(e: FinishEvent) {
    match e {
        FinishEvent::SignaturesApplied { count } => println!("Applied {count} signature(s); all verify against rk"),
        FinishEvent::Proving(zecz::authorize::ProveEvent::LoadingKey { pool, circuit }) => {
            eprintln!("Loading the {pool} proving key ({circuit}) ...")
        }
        FinishEvent::Proving(zecz::authorize::ProveEvent::Proving { pool }) => eprintln!("Proving the {pool} bundle ..."),
        FinishEvent::Stored { txid, size, .. } => println!("Transaction {txid} ({size} B) stored in the wallet"),
    }
}

async fn cmd_init(profile: Profile, spec: &AccountSpec, birthday: u32) -> Result<()> {
    let data_dir = profile.data_dir.clone();
    let (_, outcome) = Wallet::init(profile, spec, birthday, |e| match e {
        InitEvent::FetchingTreeState { height, server } => eprintln!("Fetching tree state at height {height} from {server} ..."),
    })
    .await?;
    match outcome {
        InitOutcome::AlreadyImported { addresses } => {
            println!("Account already imported in {}.", data_dir.display());
            print_addresses(&addresses);
        }
        InitOutcome::Imported(a) => {
            println!("Imported account {:?} ({}), birthday {}", a.name, a.uuid, a.birthday);
            if let Some(o) = a.origin {
                println!("Seed fingerprint {}, ZIP 32 account {}", hex::encode(o.seed_fingerprint), o.account_index);
                // The short form the device shows on its home and wallet screens.
                println!("ZFP {} (compare with the device)", o.short_fingerprint());
            }
            print_addresses(&a.addresses);
        }
    }
    Ok(())
}

async fn cmd_sync(profile: Profile, batch: u32) -> Result<()> {
    let mut w = Wallet::open(profile)?;
    let status = w
        .sync(batch, |e| match e {
            SyncEvent::Started { server, tip } => eprintln!("Syncing with {server} (chain tip {tip}) ..."),
            SyncEvent::Scanned(r) => match r.tip {
                Some(t) => eprintln!("  scanned {}..{} ({} blocks behind tip {t})", r.start, r.end, t.saturating_sub(r.end.saturating_sub(1))),
                None => eprintln!("  scanned {}..{}", r.start, r.end),
            },
        })
        .await?;
    match status {
        Some(s) => println!(
            "Synced: chain tip {}, fully scanned to {}, scan progress {}/{}",
            s.chain_tip, s.fully_scanned, s.progress.0, s.progress.1
        ),
        None => println!("Synced, but the wallet has no summary yet"),
    }
    Ok(())
}

async fn cmd_tip(profile: &Profile) -> Result<()> {
    let t = wallet::tip(&profile.server).await?;
    println!("{}: chain {:?}, tip {} ({}), server {} {}", t.server, t.chain_name, t.height, t.consensus_branch_id, t.vendor, t.version);
    // Shown, not refused: `tip` is how one finds out what a server is.
    if let Err(e) = wallet::check_chain(profile.network, &t.chain_name) {
        eprintln!("WARNING: {e}");
    }
    Ok(())
}

fn cmd_balance(profile: Profile) -> Result<()> {
    let b = Wallet::open(profile)?.balances()?;
    println!("Chain tip {}, fully scanned to {}", b.chain_tip, b.fully_scanned);
    if !b.synced {
        println!("(not fully synced; balances may be incomplete)");
    }
    let Some(b) = b.account else {
        println!("Total: {}", format_zec(0));
        return Ok(());
    };
    let pool = |label: &str, bal: &wallet::PoolBalance| {
        println!("  {label:<12} {}  (spendable {}, pending {})", format_zec(bal.total), bal.spendable, bal.pending);
    };
    pool("Orchard", &b.orchard);
    pool("Ironwood", &b.ironwood);
    pool("Transparent", &b.transparent);
    println!("Total:     {}", format_zec(b.total));
    println!("Spendable: {}", format_zec(b.spendable));
    if b.transparent.total > 0 {
        println!("Note: the device does not sign transparent inputs yet; transparent funds cannot be sent from here.");
    }
    Ok(())
}

fn cmd_address(profile: Profile) -> Result<()> {
    print_addresses(&Wallet::open(profile)?.addresses()?);
    Ok(())
}

fn cmd_send(profile: Profile, to: &str, amount: &str, zec: bool, memo: Option<&str>, out: &Path, compact: bool) -> Result<()> {
    let mut w = Wallet::open(profile)?;
    let amount_zat = if zec { parse_zec(amount)? } else { amount.parse::<u64>().context("amount must be zatoshis (or pass --zec)")? };
    let p = w.propose_send(&SendRequest { to: to.to_string(), amount_zat, memo: memo.map(str::to_string), compact })?;
    write_proposal(&p, out)
}

fn cmd_shield(profile: Profile, out: &Path, compact: bool) -> Result<()> {
    let mut w = Wallet::open(profile)?;
    let p = w.propose_shield(compact)?;
    write_proposal(&p, out)
}

fn write_proposal(p: &zecz::wallet::SendProposal, out: &Path) -> Result<()> {
    fs::write(out, &p.signer_view).with_context(|| format!("writing {}", out.display()))?;

    println!("Proposal (target height {}):", p.target_height);
    for pay in &p.payments {
        println!(
            "  pay  {} to {} [{}]{}",
            format_zec(pay.amount_zat),
            pay.recipient,
            pay.pool,
            if pay.has_memo { " with memo" } else { "" }
        );
    }
    for c in &p.change {
        println!("  change {} [{}]", format_zec(c.value_zat), c.pool);
    }
    println!("  fee  {}", format_zec(p.fee_zat));
    println!(
        "Unsigned PCZT ({} signer view, {} B) -> {}",
        if p.compact { "compact" } else { "full" },
        p.signer_view.len(),
        out.display()
    );
    println!("  sha256 {}", hex::encode(p.sha256));
    println!("Full PCZT kept for proving -> {}", p.pending_path.display());
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

/// The .zkv file passphrase: ZECZ_ZKV_PASSPHRASE, or one line of stdin.
fn zkv_passphrase() -> Result<String> {
    if let Ok(p) = std::env::var("ZECZ_ZKV_PASSPHRASE") {
        return Ok(p);
    }
    eprint!("File passphrase: ");
    let mut line = String::new();
    std::io::stdin().read_line(&mut line)?;
    Ok(line.trim_end_matches(['\r', '\n']).to_string())
}

fn cmd_zkv_new(mnemonic: &str, out: &Path, birthday: u32, account: u32, label: &str, iterations: u32) -> Result<()> {
    seed_card::check_mnemonic(mnemonic)?;
    let pass = zkv_passphrase()?;
    let card = NewCard { mnemonic, passphrase: &pass, birthday, account, label, iterations };
    let size = seed_card::write_new(&card, out)?;
    println!("Wrote {} ({} bytes, {} iterations)", out.display(), size, iterations);
    Ok(())
}

fn cmd_zkv_open(profile: &Profile, input: &Path, bip39_pass: &str) -> Result<()> {
    let file = fs::read(input)?;
    let c = seed_card::describe(&file, &zkv_passphrase()?, bip39_pass, profile.network)?;
    println!("{}: {} words, {} iterations", input.display(), c.words, c.iterations);
    println!("Label {:?}, account {}, birthday {}, flags {:#04x}", c.label, c.account, c.birthday, c.flags);
    println!("ZFP {}", c.short_fingerprint);
    println!("UFVK: {}", c.ufvk);
    Ok(())
}

async fn cmd_device_init(profile: Profile, port: &str, account: u32, birthday: Option<u32>, name: &str) -> Result<()> {
    let net = profile.net_name();
    let mut dev = connect_device(port)?;
    let a = dev.z_get_account(account, net)?;
    if device::get_text(&a, "network")? != net {
        bail!("the device answered for a different network");
    }
    let ufvk = device::get_text(&a, "ufvk")?;
    let fp: [u8; 32] = device::get_bytes(&a, "seedFingerprint")?
        .try_into()
        .map_err(|_| anyhow!("seedFingerprint is not 32 bytes"))?;
    let device_ua = device::get_text(&a, "ua")?;
    let device_birthday = device::get_uint(&a, "birthday").ok().map(|b| b as u32);
    let birthday = match (birthday, device_birthday) {
        (Some(b), _) => b,
        (None, Some(b)) => {
            println!("Birthday {b}, from the device");
            b
        }
        (None, None) => bail!("the device does not know this account's birthday; pass --birthday"),
    };
    println!("Device account {account}: {device_ua}");
    println!("Device t-address: {}", device::get_text(&a, "tAddress")?);

    // The device derives its address itself; the UFVK must lead to the same one.
    let derived = wallet::addresses_for_ufvk(profile.network, &ufvk).context("device UFVK")?;
    if derived.unified != device_ua {
        bail!("the device's address does not match its own viewing key; refusing to import");
    }
    let spec = AccountSpec::device(ufvk, fp, account, name);
    let net = profile.net_name();
    cmd_init(profile, &spec, birthday).await?;
    if device_birthday != Some(birthday) {
        let stored = dev.z_set_birthday(account, net, birthday)?;
        println!("Recorded birthday {birthday} on the device{}",
                 if stored { "" } else { " until it locks (passphrase or session seed)" });
    }
    Ok(())
}

fn cmd_device_show_address(profile: &Profile, port: &str, account: u32) -> Result<()> {
    let mut dev = connect_device(port)?;
    // The address this wallet would hand out, from the device's own UFVK.
    let a = dev.z_get_account(account, profile.net_name())?;
    let ua = wallet::addresses_for_ufvk(profile.network, &device::get_text(&a, "ufvk")?).context("device UFVK")?.unified;
    println!("This wallet's address:\n  {ua}\nCompare it with the device and answer there.");
    dev.z_show_address(account, profile.net_name(), &ua)?;
    println!("Confirmed on the device.");
    Ok(())
}

fn cmd_sign_device(profile: &Profile, port: &str, account: u32, input: &Path, out: &Path) -> Result<()> {
    let bytes = fs::read(input).with_context(|| format!("reading {}", input.display()))?;
    let mut dev = connect_device(port)?;
    println!("Uploading {} bytes (sha256 {}) ...", bytes.len(), hex::encode(zsig::sha256(&bytes)));
    println!("Review the transaction on the device; ACCEPT on its last page signs.");
    let (sighash, entries) = dev.z_sign(&bytes, account, profile.net_name(), |done, total| {
        eprint!("\r  {done}/{total} bytes");
        if done == total {
            eprintln!();
        }
    })?;
    let z = Zsig::new(&bytes, entries);
    fs::write(out, z.to_bytes()?)?;
    println!("Device signed {} spend(s); sighash {}", z.entries.len(), hex::encode(sighash));
    for e in &z.entries {
        let what = if e.pool == zsig::Pool::Transparent { "input" } else { "action" };
        println!("  {:?} {what} {}", e.pool, e.index);
    }
    println!("Signatures -> {}", out.display());
    Ok(())
}

fn cmd_sign_soft(profile: &Profile, mnemonic: &str, passphrase: &str, account: u32, input: &Path, out: &Path) -> Result<()> {
    eprintln!("WARNING: sign-soft is a DEV ONLY reference signer. It does not check what it signs.");
    let bytes = fs::read(input).with_context(|| format!("reading {}", input.display()))?;
    let acct = DevAccount::from_mnemonic(&profile.network, mnemonic, passphrase, account)?;
    let z = soft_signer::sign(&bytes, &acct, profile.coin_type())?;
    fs::write(out, z.to_bytes()?)?;
    println!("Signed {} spend(s) of {} (sha256 {})", z.entries.len(), input.display(), hex::encode(z.pczt_sha256));
    for e in &z.entries {
        let what = if e.pool == zsig::Pool::Transparent { "input" } else { "action" };
        println!("  {:?} {what} {}", e.pool, e.index);
    }
    println!("Signatures -> {}", out.display());
    Ok(())
}

async fn cmd_finish(profile: Profile, pczt_path: &Path, sigs_path: &Path, no_broadcast: bool) -> Result<()> {
    let mut w = Wallet::open(profile)?;
    let f = w.finish_files(pczt_path, sigs_path, !no_broadcast, print_progress).await?;
    if no_broadcast {
        println!("Not broadcast (--no-broadcast). Raw tx: {}", f.raw_path.display());
    } else {
        println!("Broadcast OK: txid {}", f.txid);
    }
    Ok(())
}

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();
    let network = match cli.network {
        Net::Test => Network::TestNetwork,
        Net::Main => Network::MainNetwork,
    };
    let profile = Profile::new(network, cli.data_dir, cli.server);

    match cli.cmd {
        Cmd::Init { ufvk, birthday, seed_fingerprint, account_index, name } => {
            let fp: [u8; 32] = hex::decode(&seed_fingerprint)
                .ok()
                .and_then(|b| b.try_into().ok())
                .ok_or_else(|| anyhow!("--seed-fingerprint must be 32 bytes of hex"))?;
            let spec = AccountSpec::device(ufvk, fp, account_index, &name);
            cmd_init(profile, &spec, birthday).await
        }
        Cmd::InitDev { mnemonic, passphrase, account, birthday, name } => {
            eprintln!("WARNING: init-dev is DEV ONLY. It handles a mnemonic on the host; never use it with real funds.");
            let acct = DevAccount::from_mnemonic(&network, &mnemonic, &passphrase, account)?;
            let spec = AccountSpec::from_dev_account(network, &acct, &name)?;
            println!("UFVK: {}", spec.ufvk);
            cmd_init(profile, &spec, birthday).await
        }
        Cmd::ZkvNew { mnemonic, out, birthday, account, label, iterations } => {
            cmd_zkv_new(&mnemonic, &out, birthday, account, &label, iterations)
        }
        Cmd::ZkvOpen { input, passphrase } => cmd_zkv_open(&profile, &input, &passphrase),
        Cmd::Sync { batch } => cmd_sync(profile, batch).await,
        Cmd::Tip => cmd_tip(&profile).await,
        Cmd::Balance => cmd_balance(profile),
        Cmd::Address => cmd_address(profile),
        Cmd::Send { to, amount, zec, memo, out, compact } => cmd_send(profile, &to, &amount, zec, memo.as_deref(), &out, compact),
        Cmd::Shield { out, compact } => cmd_shield(profile, &out, compact),
        Cmd::DeviceInit { port, account, birthday, name } => cmd_device_init(profile, &port, account, birthday, &name).await,
        Cmd::DeviceShowAddress { port, account } => cmd_device_show_address(&profile, &port, account),
        Cmd::SignDevice { port, account, input, out } => cmd_sign_device(&profile, &port, account, &input, &out),
        Cmd::SignSoft { mnemonic, passphrase, account, input, out } => {
            cmd_sign_soft(&profile, &mnemonic, &passphrase, account, &input, &out)
        }
        Cmd::Finish { pczt, sigs, no_broadcast } => cmd_finish(profile, &pczt, &sigs, no_broadcast).await,
    }
}
