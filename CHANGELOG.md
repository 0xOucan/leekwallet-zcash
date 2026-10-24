# Changelog

Each release is one tag in this repository; its files, checksums and how to
verify or rebuild them are in [RELEASES.md](RELEASES.md). The Zodl fork's APKs
are released in
[leekwallet-zcash-android](https://github.com/0xOucan/leekwallet-zcash-android)
(LEEKWALLET-RELEASE.md there).

## v0.1.0 "Sacbeh" (unreleased)

*Sacbeh*: from the Maya *sak bej*, "white road", the raised limestone
causeways that joined Maya cities across the jungle.

The first release, built during ZECATHON on top of the author's earlier
LeekWallet for Ethereum (tag `pre-zecathon`), with every EVM part removed.

- **Firmware** for the ESP32-S3 CAM board: a PCZT (ZIP 374) signer for
  Orchard and transparent inputs that checks every spend is its own,
  recomputes the sighash, shows each output, amount and memo, and returns
  only signatures. It verifies addresses on its own screen and refuses
  requests for the other network. Links: USB and Bluetooth (encrypted,
  confirmed by a six-digit code), and the SD card.
- **Zcash LeekWallet**, one app for Linux, Windows, macOS and Android
  (64-bit phones only): several wallets, mainnet or testnet, contacts, memos,
  QR pay and NEAR Intents swaps, every send signed on the board.
- **Chrome extension**: the same wallet in the browser, over USB or
  Bluetooth.
- **Releases**: `SHA256SUMS` signed with the maintainer's SSH key
  ([allowed_signers](allowed_signers), the key at
  <https://github.com/0xOucan.keys>). The firmware rebuilds byte-identical
  from the tag.

Not in this release: the firmware flasher inside the desktop app (flash from
the website's flasher or with esptool). Not audited; no secure boot or flash
encryption, so small amounts only.
