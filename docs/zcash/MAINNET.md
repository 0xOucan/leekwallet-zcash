# First mainnet test (roadmap 10.3)

A small amount of real ZEC through the board: receive, verify the address,
send some back, shield a transparent coin. Once with `zecz`, once with the
desktop app. Everything here was run on the PC first (sim, zdev, a
watch-only mainnet sync); this checklist is for the board.

## Before you start

- **Firmware.** The board runs a build with transparent inputs (roadmap
  10.1, commit `ad54431` or later; `main` at `44c2f8a` is fine). No
  firmware change is needed for mainnet: the network travels in every
  request. Its signers accept branch `0x37a5165b` (NU6.3), which mainnet
  is on; NU7 (`0x77190ad9`) is testnet only.
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
- **On-device Receive** (no companion): Settings → `Net Mainnet` first;
  the receive pages then say `MAIN`. USB and Bluetooth requests do not
  depend on this setting.

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
   wallet; switching to Mainnet again reopens the mainnet one.

Record each txid, the link (USB/BLE) and the companion in
`docs/JOURNAL.md`.

## When something fails

- **Nothing was signed** unless the device showed ACCEPT and you pressed
  it. A refusal before review names its reason: `-17` is the other
  network (wrong `--network` or app network), `-12` is not this seed's
  account. Fix the network and start again.
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
