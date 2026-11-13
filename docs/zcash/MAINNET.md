# Mainnet

## NU7

NU7 (ZIP 259, branch `0x77190ad9`) activates on mainnet at block 3,543,405,
about 02:20 UTC on 2026-11-06.

- **Apps must be updated before it.** The old host code has no mainnet height
  for NU7, so past it it builds transactions for NU6.3, the board signs them,
  and nodes reject them. The new crates (`zcash_primitives 0.31.0` and
  siblings) set the height.
- **Update the boards too.** The new firmware is a suggestion, not a
  requirement: a v0.1.0 board signs NU7 in the simulator suites, and new apps
  still use it. The new firmware adds the checks under "Version notices", and
  what v0.1.0 lacks: "Share view key?" before the UFVK, and the refusal of a
  repeated rk (on v0.1.0 only the host's preflight stops that). **Tested on a
  board on testnet** (2026-11-10): a NU7 send over USB from `zecz`, txid
  `8676ee562b8066b8c9fb720706551f2a57f8176066380bfd29d5edf50abd0151`, mined in block 4,606,165. Since then the whole stack has signed NU7 sends
  on testnet over USB, Bluetooth and QR (the first QR send, both ways, from
  `zecz`: `8f3b9264fddce48f6989396336741512e3fdafe66870826c0ceeb63725e73c7d`,
  block 4,606,736), from Zingo, Zodl, the companion and `zecz`. Not yet:
  mainnet, SD and a shield on NU7. The crates did not change the PCZT wire
  format: regenerating `vectors/` with them leaves all 42 existing vectors and
  `keys.json` byte-identical (with `iw_send_nu7`'s old 40-block expiry pinned).
- **Wallet databases do not change.** `zcash_client_sqlite 0.23.0` has the same
  75 migrations as the rev v0.1.0 used, so a v0.1.0 database opens as it is
  (`host/zecz-core/tests/old_db.rs`), and v0.1.0 can still open it afterwards.
  Going back is still no use after the activation: that app builds for NU6.3.
- **Blocks are 25 s** instead of 75 s. The apps sync every 25 s in front. A new
  transaction expires 120 blocks after its target height, about 50 minutes,
  the same time as 40 blocks before NU7. The review shows the rules (`NU7`) and
  the expiry height.
- **A transaction signed before block 3,543,405 can never be mined after it.**
  It commits to its branch. The app checks before proving, before storing and
  before each broadcast, and says so: nothing was sent, anything it spends is
  free again after its expiry height, and the send is built and signed again. A
  transaction already stored shows as pending, then "expired, never mined".
- **v4 is invalid from NU7** (ZIP 2003). Nothing here builds v4: every PCZT is
  v6 from NU6.3 on.

### Version notices

Guidance for the user, not a security boundary: either side can lie, and no
check on the board depends on them. The 6-digit pairing code, "Share view key?",
the review and the absence of remembered hosts do not change.

| Who says it | What it says | When | What it means |
|---|---|---|---|
| Any new app, `zecz` | "Update your board: ..." | `zGetFeatures` reports no `fwCompat`, or less than `MIN_FIRMWARE_COMPAT` (2) | The board's firmware is older. It can still sign. Nothing is blocked. |
| The new board, for a few seconds | "Update your app" | Once a session, when a request from the app has no `hostCompat`, or less than 2 | The app is older than the board. Everything carries on; any button dismisses the notice and does nothing else. |
| The new board, and the app | "Update your app" and error `0x0519` (SDK code 520) | A PCZT on branch NU6.3 with expiry at or above NU7 + 40 | An app without NU7 built it. Nothing is signed. Update the app and build it again. |

**How the refusal works.** The board has no clock, but a transaction carries its
expiry. An app without NU7's mainnet height keeps building for NU6.3 past the
activation, with expiry target height + 40. Target height A (the activation) gives
A + 40, so both signers refuse NU6.3 from there: mainnet from expiry 3,543,445,
testnet from 4,465,066. A transaction for the last NU6.3 block, A - 1, has expiry
A + 39, can still be mined, and passes. It can miss: an old app whose view of the
chain is behind builds a lower expiry and is not caught, and its transaction fails
at the node as before. Expiry 0 (none) is not refused; no app here builds it.

**How the levels travel.** `zGetFeatures` answers `fwCompat`. Hosts send
`hostCompat` with `zGetSeedFingerprint`, `zGetAccount` and `zSignBegin`; old
firmware ignores it. An old app sends nothing, which is how the board knows.

**What has been run:** the suites in `docs/JOURNAL.md` under "NU7", on the PC.
**Not run yet:** any of this on a real board, including the new firmware, or
against mainnet after the activation. A first send after block 3,543,405 has to be done by hand, with the
checklist below.

## First mainnet test (roadmap 10.3)

A small amount of real ZEC through the board: receive, verify the address,
send some back, shield a transparent coin. Once with `zecz`, once with the
desktop app. Everything here was run on the PC first (sim, zdev, a
watch-only mainnet sync); this checklist is for the board.

## Before you start

- **Firmware.** The board runs a build with transparent inputs (roadmap
  10.1, commit `ad54431` or later) and the network guard (branch
  `board-network-guard`, 2026-10-17, or later). The network travels in
  every request, and the board answers only for its own: see the next
  point. Its signers accept branch `0x37a5165b` (NU6.3) and `0x77190ad9`
  (NU7, on mainnet from block 3,543,405). See "NU7" above: the apps must be updated, and
  the board should be.
- **The board's network.** On the board, Settings → `Net Mainnet` (the
  out-of-the-box setting; it is persisted). The board refuses every
  request for the other network (error `0x0528`), and says so on its
  screen ("Wrong network", the app's network and its own): the board's
  setting must match the app's. A board left on Testnet refuses the
  mainnet app with "The board is set to Testnet, but this app is on
  Mainnet. Switch the board to Mainnet in Settings → Network, or use the
  Testnet app." Before this guard, a board on Testnet answered the mainnet
  app anyway, so the setting said one network while the app used the
  other.
- **Backup.** The 24 words of the seed on the board are written down on
  paper, checked word by word against the device, and kept offline. Note
  the ZFP (seed fingerprint) from the home screen and any passphrase.
  Without them a dead board means lost funds.
- **Amounts.** Small: about 0.003 ZEC in total. A fee is 10,000-20,000
  zat (0.0001-0.0002 ZEC). Never more than you would lose.
- **Separate data.** Mainnet gets its own wallet folder: `--data-dir
  ~/zecz-main` for zecz (never a testnet folder; zecz refuses a folder of
  the other network anyway), and the desktop app keeps
  `wallets/main` apart from `wallets/test`.
- **Server.** `https://zec.rocks:443` (the default). Check it:

  ```sh
  zecz --network main tip
  # https://zec.rocks:443: chain "main", tip 3520xxx (37a5165b), ...
  ```

  zecz and the app refuse a server whose chain is not the wallet's.

## What the device must show

- **Address check** (zShowAddress): the address in full, then
  "Is this the address the app shows?" with `acct 0  MAIN`. The address
  starts with `u1`; a transparent one with `t1`.
- **Review pages**: amounts in **ZEC** (never TAZ); the "Fee & network"
  page says **Network: Mainnet** and **Upgrade: NU6.3**; a shield shows one
  "From k of n" page per coin with its `t1…` address and its
  `Path .../scope/index` (the device re-derives the key under coin type 133).
- **On-device Receive** (no companion): with Settings → `Net Mainnet`,
  the receive pages say `MAIN`. The same setting decides which network
  USB and Bluetooth requests are answered for.

Anything that says Testnet, TAZ, `utest`/`tm`, or coin type 1: press NO
and stop.

## With zecz (USB)

Build once: `cd host && cargo build --release -p zecz-cli`; below,
`zecz` is `host/target/release/zecz` and the port is the board's
(`/dev/ttyACM0`).

1. **Import.** Birthday = the tip from `zecz --network main tip`.

   ```sh
   zecz --network main --data-dir ~/zecz-main device-init --port /dev/ttyACM0 --birthday <tip>
   ```

   Compare the passkey, press ALLOW. It prints the `u1…` address, the
   `t1…` address and the ZFP: the ZFP must match the device's.
2. **Verify the address** before anyone pays it:

   ```sh
   zecz --network main --data-dir ~/zecz-main device-show-address --port /dev/ttyACM0
   ```

   Compare every character on the device (`acct 0  MAIN`), press YES.
3. **Receive.** Send about 0.002 ZEC from another wallet to the `u1…`
   address, and 0.001 ZEC to the `t1…` address (for step 5). Wait for a
   confirmation, then:

   ```sh
   zecz --network main --data-dir ~/zecz-main sync
   zecz --network main --data-dir ~/zecz-main balance
   ```
4. **Send back** a small amount to an address you control elsewhere:

   ```sh
   zecz --network main --data-dir ~/zecz-main send --to <your u1 address> --amount 0.0005 --zec --memo "LeekWallet mainnet" --out /tmp/main-send.pczt --compact
   zecz --network main --data-dir ~/zecz-main sign-device --port /dev/ttyACM0 --in /tmp/main-send.pczt --out /tmp/main-send.zsig
   zecz --network main --data-dir ~/zecz-main finish --pczt /tmp/main-send.pczt --sigs /tmp/main-send.zsig
   ```

   On the device: recipient, 0.0005 ZEC, memo, change, fee, Network:
   Mainnet; ACCEPT on the last page. `finish` prints the txid; look it up
   on a mainnet explorer and see it arrive in the other wallet.
5. **Shield** the transparent coin once it is confirmed and synced:

   ```sh
   zecz --network main --data-dir ~/zecz-main shield --out /tmp/main-shield.pczt --compact
   zecz --network main --data-dir ~/zecz-main sign-device --port /dev/ttyACM0 --in /tmp/main-shield.pczt --out /tmp/main-shield.zsig
   zecz --network main --data-dir ~/zecz-main finish --pczt /tmp/main-shield.pczt --sigs /tmp/main-shield.zsig
   ```

   The device shows the coin (`t1…`, `Path .../0/0`) and "Shield …
   into this wallet". After a sync the transparent balance is 0 and
   Ironwood holds the rest minus the fee.

## With the desktop app (USB or Bluetooth)

1. Open "Network and server", choose **Mainnet**, confirm "Switch to
   mainnet". The red **MAINNET — real funds** bar appears and stays; the
   badge says mainnet; the server is `https://zec.rocks:443`. The testnet
   wallet is untouched in its own folder.
2. Connect the board (passkey, ALLOW), import with the chain tip as the
   birthday, and check the ZFP.
3. Receive: "Verify on device" and compare (`MAIN` on the device). Sync
   after a payment arrives.
4. Send 0.0005 ZEC back with a memo: Review, then tick **"This sends real
   ZEC on mainnet…"** (Sign stays disabled until then), Sign on
   LeekWallet, check the device pages, ACCEPT. Note the txid.
5. Shield the transparent funds the same way (its own tick box).
6. Switching back to Testnet disconnects the board and reopens the testnet
   wallet; switching to Mainnet again reopens the mainnet one. The board
   must be switched too (Settings → Network on the board), or it refuses
   the app's requests and the app says which setting to change.

Record each txid, the link (USB/BLE) and the companion in
`docs/JOURNAL.md`.

## When something fails

- **Nothing was signed** unless the device showed ACCEPT and you pressed
  it. A refusal before review names its reason: `-17` is a PCZT of the
  other network (wrong `--network` or app network), `-12` is not this
  seed's account. Fix the network and start again.
- **"The board is set to Testnet, but this app is on Mainnet"** (error
  `0x0528`, also "Wrong network" on the board): the board's Settings →
  Network differs from the app's. Nothing was derived or signed. Switch
  the board (or the app) and connect again.
- **Signed but not broadcast** (finish failed, app error after signing):
  the transaction is in the wallet and in `<data dir>/sent/<txid>.tx`.
  Nothing moves until it is mined; if it never is, it expires about 40
  blocks after its target height and the funds show again after a sync.
  Build a new one rather than reusing old files.
- **Balance looks wrong**: sync again; check the server is mainnet (`zecz
  --network main tip`). A wallet folder can be deleted and re-imported
  from the device with the same birthday: it only holds what the chain
  and the device already know.
- **Server of the other chain**: zecz and the app refuse it ("follows the
  "test" chain"). Use `https://zec.rocks:443` or press "Use the default".
- **Board lost or broken**: the funds are on the seed, not the board.
  Restore the 24 words (and passphrase) onto a reflashed LeekWallet and
  import again; account 0, coin type 133. Any other wallet must support
  NU6.3 (Ironwood) to see funds received or shielded after it.

## A tiny NEAR Intents swap (roadmap 8.6)

From the desktop app, with the board, on the mainnet wallet above. The
swap service is NEAR Intents' 1Click: the app cannot check its rate or
its deposit address, the board shows only what it pays.

**Before.** The mainnet wallet is synced and holds at least 0.004 ZEC
spendable (shielded). You have an address of your own on the other chain
(say USDC on Solana, or on NEAR) that can receive the asset; nobody
else's. Amounts: 1Click's minimum for ZEC → USDC was 0.00132 ZEC on
2026-10-17; use **0.003 ZEC** (about the price of a coffee). A refund
would cost about 0.00032 ZEC. No API key is needed (1Click then adds
0.25%); leave "Confidential" off.

1. **Preview.** Swap card: "ZEC → another asset", pick the asset, amount
   0.003, your address on that chain, slippage 1%. "Preview price" (dry
   run): the rate, the minimum received and "about N min" appear. Nothing
   is created.
2. **Quote.** "Get quote". Check: you pay 0.003 ZEC; you receive about
   …; at least …; recipient = your address (character by character);
   refunds to a `u1…` address of this wallet; "Deposit by" about two
   hours ahead. Note the deposit address and whether the card says
   transparent (`t1…`) or unified (`u1…`): record which in the journal.
3. **Review the deposit.** The review shows one payment of exactly 0.003
   ZEC to that deposit address, the change and the fee. Tick the
   mainnet box, "Sign on LeekWallet", ACCEPT on the board after checking
   on its pages: the deposit address (a `t1…` one comes with the
   transparent warning), 0.003 ZEC, the fee, Network: Mainnet. Note the
   txid.
4. **Follow it.** The swap in the list goes "Deposit sent" → "Deposit
   seen" (after about one block) → "Swapping" → "Done", polled every 20 s
   ("Check now" polls at once). It shows the payout transaction on the
   other chain; check it arrived in your wallet there. The deposit
   address can also be looked up on explorer.near-intents.org.
5. **If it is refunded** (rate moved past the slippage, or the deposit
   came late): the state says Refunded with the reason; the ZEC (minus
   the refund fee) comes back to the `u1…` refund address; sync to see
   it. Nothing else to do.
6. **Optional, into ZEC.** "Another asset → ZEC", e.g. 2 USDC, your
   refund address on that chain, "Get quote": the card shows a deposit
   address on that chain with a QR code and the deadline. Pay exactly
   that amount from your wallet there, before the deadline, nothing
   else. The swap goes to Done and the ZEC arrives shielded; sync.

Stop and press NO on the board if its address differs from the app's,
the amount is not the quoted one, or it says Testnet. Record in
docs/JOURNAL.md the route, the amounts, the deposit address type and the
time it took; keep txids and addresses out of the repository.

The swaps are kept in `<app data dir>/swaps-main.json` (quote, 1Click's
signature, deposit txid, last status), so a restart keeps following
them; keep the file until the swap is done: it is what a dispute needs.
