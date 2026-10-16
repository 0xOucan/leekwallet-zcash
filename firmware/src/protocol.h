/**
 * LeekWallet protocol endpoint - see protocol.c and docs/PROTOCOL.md.
 *
 * The command layer is transport-blind: USB and BLE both hand complete frames
 * to protocol_handle_frame() and both get their replies through the installed
 * writer. Only one transport is live at a time (PROTOCOL.md 3b), so there is
 * one writer, not a list.
 */

#ifndef LEEK_PROTOCOL_H
#define LEEK_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Largest frame the device will assemble in either direction.
 *
 * 4096, the size docs/PROTOCOL.md 2 always allowed and both host transports
 * already accept. LeekWallet held 1024 for EIP-712; Zcash sends a PCZT of
 * several to hundreds of kilobytes as a sequence of zSignChunk frames, and
 * four times the frame is a quarter of the round trips (and of the BLE
 * chunk headers). The cost is static RAM: rx_buf and tx_buf, 8 KB together.
 *
 * The declared length is still checked against this bound before a single
 * byte is buffered, so the peer cannot dictate an allocation.
 */
#define PROTOCOL_MAX_FRAME 4096

/*
 * USB-Serial-JTAG driver buffers. The driver drops (returns 0 for) any single
 * write larger than its TX ring buffer, and its RX interrupt drops bytes when
 * the RX ring is full. At 256 each - LeekWallet's values, sized for short
 * Ethereum replies - every Zcash account reply (~700 bytes) vanished on the
 * first board. Frames now go out in pieces of at most PROTOCOL_USB_TX_CHUNK,
 * and RX holds two whole frames.
 */
#define PROTOCOL_USB_TX_BUF    1024
#define PROTOCOL_USB_TX_CHUNK  512
#define PROTOCOL_USB_RX_BUF    (2 * PROTOCOL_MAX_FRAME)

/**
 * The wire protocol this firmware speaks, offered and required in `hello`.
 *
 * v1 was the single-round-trip handshake whose passkey a relay could grind
 * offline (docs/AUDIT-TRANSPORT.md C-1); v2 is the commit-then-reveal one that
 * replaced it. The two are not compatible and must not be allowed to look
 * compatible: v1's `hello` carried a version the client never read, so a
 * mismatched pair would have negotiated half a handshake and then failed on
 * the first encrypted frame with "decrypt failed", which says nothing true
 * about what is wrong.
 *
 * So both ends check, and both ends name the versions in the message. A host
 * that offers anything but this number is refused at `hello` with ERR_VERSION,
 * and a host that gets back anything but this number stops before deriving.
 * Old firmware answers v1 and is caught by the second rule; old hosts send no
 * version at all and are caught by the first.
 */
#define PROTOCOL_VERSION 2

/** Install the USB-Serial-JTAG driver and start the listener task. */
void protocol_start(void);

/**
 * Handle one complete frame: len:u16 ‖ type ‖ body, big-endian, with no sync
 * marker and no chunk headers. Must be writable — encrypted bodies are
 * decrypted in place.
 */
void protocol_handle_frame(uint8_t *frame, size_t len);

/**
 * Refuse a complete request the transport could not hand over, in plaintext.
 *
 * For transports that queue rather than dispatch inline. Every complete frame
 * must produce a reply of some kind; a transport that drops one silently
 * disagrees with the cable, which is the class of bug this exists to prevent.
 */
void protocol_send_transport_busy(void);

/** Sink for outgoing frames, in the same marker-free form. */
typedef void (*ProtocolWriter)(const uint8_t *frame, size_t len);

/** Route replies elsewhere; NULL restores the USB endpoint. */
void protocol_set_writer(ProtocolWriter writer);

/** Forget any partially received frame, as a transport switch must. */
void protocol_reset_rx(void);

/**
 * Whether the USB port is an endpoint. Cleared while BLE is the selected
 * transport, so the cable is drained but never answered (PROTOCOL.md 3b).
 */
void protocol_set_rx_enabled(bool enabled);
bool protocol_rx_enabled(void);

/**
 * Tell the endpoint the user typed a passphrase on the device itself (T42).
 *
 * A host-supplied passphrase is scoped to the session that supplied it and is
 * dropped when that session ends. A passphrase entered on the device is not:
 * it belongs to the person holding the device, and a BLE disconnect must not
 * silently return them to the base wallet. This is how the endpoint learns
 * that whatever it applied earlier has been superseded by a passphrase it
 * has no claim over.
 */
void protocol_note_device_passphrase(void);

#endif /* LEEK_PROTOCOL_H */
