#!/usr/bin/env python3
"""
Mutation check for the BLE transport tests.

A test suite that passes is evidence of nothing until you have watched it fail.
Each mutant below is a plausible mistake in the code this change adds — an
off-by-one in the chunk capacity, a dropped bound, a sync marker on the wrong
transport, a transport switch that forgets the session. If a mutant survives,
the corresponding test is decorative.

  python3 sim/mutants.py
"""

import subprocess, shutil, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MUTANTS = [
    # file, find, replace, which test binaries should notice
    ("src/ble-chunk.c", "size_t capacity = (size_t)mtu - 3 - 1;",
     "size_t capacity = (size_t)mtu - 3;", "chunk larger than the MTU allows"),
    ("src/ble-chunk.c", "if (seq != r->next_seq) {",
     "if (false) {", "out-of-order chunks accepted"),
    ("src/ble-chunk.c", "if (total < 4 || total > sizeof(r->buf) || r->len > total) {",
     "if (false) {", "declared length unbounded"),
    ("src/ble-chunk.c", "if (r->len + body > sizeof(r->buf)) {",
     "if (false) {", "reassembly buffer unbounded"),
    ("src/ble-chunk.c", "if (!more && r->len != total) {",
     "if (false) {", "trailing/truncated bodies accepted"),
    ("src/ble-chunk.c", "r->next_seq = (uint8_t)((r->next_seq + 1) & BLE_CHUNK_SEQ_MASK);",
     "r->next_seq = (uint8_t)(r->next_seq + 1);", "sequence wrap wrong"),
    ("src/ble-chunk.c", "if (body == 0) {", "if (false) {",
     "payload-free chunks accepted"),
    ("src/ble-chunk.c", "if (mtu < 5) {", "if (mtu < 1) {",
     "unusable MTU accepted"),
    ("src/protocol.c", "if (tx_writer) {", "if (false) {",
     "BLE replies go to the cable"),
    ("src/protocol.c",
     "    if (!rx_enabled) {", "    if (false) {",
     "USB answers while BLE is selected"),
    ("src/protocol.c", "static bool rx_enabled = false;",
     "static bool rx_enabled = true;",
     "endpoint answers before a transport is chosen"),
    ("src/transport.c", "    session_reset();\n    protocol_reset_rx();",
     "    protocol_reset_rx();", "session survives a transport switch"),
    ("src/transport.c", "        ble_transport_stop();               /* advertising off, not just idle */",
     "        /* mutant: left advertising */", "BLE keeps advertising on USB"),
    ("src/transport.c", "        protocol_set_rx_enabled(false);     /* USB endpoint stops answering */",
     "        protocol_set_rx_enabled(true);", "cable stays live under BLE"),

    # The blind-signing setting (T16). Nothing consults it until the Zcash
    # signing path lands, but it is still stored, reported and switched on
    # behind a warning, and each of those can still go wrong.
    ("src/protocol.c", "cbor_write_uint(&w, blind_signing_enabled() ? 1 : 0);",
     "cbor_write_uint(&w, 0);", "getFeatures hides the setting"),
    ("src/blind-signing.c", "        enabled = (stored == 1);", "        enabled = true;",
     "any stored byte turns blind signing on"),
    ("src/blind-signing.c", "    enabled = false;      /* the default",
     "    enabled = true;       /* the default", "blind signing defaults to on"),
    ("src/ui.c", "if (++blind_confirm_count >= BLIND_CONFIRM_PRESSES) {",
     "if (++blind_confirm_count >= 1) {", "one press enables blind signing"),

    # Job 1: a reply must match the frame type of the request that caused it.
    # Each of these is the old, state-driven behaviour, or its mirror.
    ("src/protocol.c",
     "if (allow_encrypt && reply_encrypted && session_state() == SESSION_ACTIVE) {",
     "if (allow_encrypt && session_state() == SESSION_ACTIVE) {",
     "a plaintext request gets an encrypted error"),
    ("src/protocol.c",
     "    if (reply_encrypted && session_state() == SESSION_ACTIVE) {\n        int enc = session_encrypt(out, w.length, sizeof(out));",
     "    if (session_state() == SESSION_ACTIVE) {\n        int enc = session_encrypt(out, w.length, sizeof(out));",
     "a plaintext request gets an encrypted response"),
    ("src/protocol.c",
     "            reply_encrypted = true;\n            dispatch(payload, (size_t)plain);",
     "            dispatch(payload, (size_t)plain);",
     "an encrypted request gets a plaintext reply"),
    ("src/protocol.c",
     "    return reply_encrypted && session_state() == SESSION_ACTIVE;",
     "    return session_state() == SESSION_ACTIVE;",
     "a plaintext frame rides someone else's session"),

    # Job 2: no transport may answer a complete request with silence.
    ("src/protocol.c", '    send_error_ex(ERR_BUSY, "device busy; resend", false);',
     "    /* mutant: dropped on the floor */",
     "a request the transport cannot queue is dropped"),
    # Not a mutant of the busy path's `false` argument to send_error_ex: that
    # call is guarded by reply_encrypted as well, so flipping it is equivalent
    # code and would survive by construction rather than for want of a test.
    # The two `reply_encrypted = false` clears are not mutated for the same
    # reason: each is redundant with a guard that already forces plaintext, and
    # a mutant that cannot change behaviour measures nothing.
    ("src/protocol.c",
     "    reply_encrypted = false;\n    warn_on_thin_stack();",
     "    reply_encrypted = true;\n    warn_on_thin_stack();",
     "every reply is encrypted regardless of the request"),

    # Job 3: the name bound is what keeps the radio advertising at all.
    ("src/ble-name.c", "    if (len == 0 || len > BLE_NAME_MAX_LEN) {",
     "    if (len == 0) {", "an over-long name is accepted"),
    ("src/ble-name.h", "#define BLE_NAME_MAX_LEN 29", "#define BLE_NAME_MAX_LEN 40",
     "the bound is raised past the scan response"),
    ("src/ble-name.c", "        if (c < 0x20 || c > 0x7E) {", "        if (false) {",
     "control bytes in the advertised name"),
    ("src/ble-name.c", "    if (ble_name_is_valid(stored)) {",
     "    if (stored[0] != '\\0') {", "a stored name reaches the radio unchecked"),
    ("src/ble-name.c", "    if (!ble_name_is_valid(n)) {", "    if (false) {",
     "ble_name_set stores whatever it is given"),

    # T60: the passphrase selector. Entry cost is a security property here -
    # an expensive selector is what pushes people to short passphrases - so the
    # press budget is a mutant target like any other bound.
    ("src/text-entry.c",
     "    if (text_entry_on_group(e)) {\n        e->in_group = true;\n        return TEXT_ENTRY_CONTINUE;\n    }",
     "    if (false) {\n        e->in_group = true;\n        return TEXT_ENTRY_CONTINUE;\n    }",
     "opening a block types a character"),
    ("src/text-entry.c", "    e->in_group = false;\n\n    switch (option) {",
     "    /* mutant: the block stays open */\n\n    switch (option) {",
     "a block stays open after a character"),
    ("src/text-entry.c", "    if (text_entry_group_size(e) > 0 && e->in_group) {",
     "    if (false) {", "CANCEL inside a block deletes a character"),
    ("src/text-entry.c", "    while (g * g < n) {\n        g++;\n    }\n    return g;",
     "    return 12;", "blocks are a fixed size rather than sqrt(n)"),
    ("src/text-entry.c",
     "            return (e->set == TEXT_SET_SYMBOL) ? TEXT_ENTRY_MODE_CAPS\n                                               : TEXT_ENTRY_MODE_NUM;",
     "            return TEXT_ENTRY_MODE_NUM;",
     "a set offers a switch to itself, and one set is two switches away"),
    ("src/text-entry.c", "    if (text_entry_on_group(e)) {\n        int blocks = (n + g - 1) / g;",
     "    if (false) {\n        int blocks = (n + g - 1) / g;",
     "a block is labelled with one of its characters"),
    # The passphrase ring is meant to stay flat whatever the setting says, so
    # the mutation is letting the setting through - the old behaviour.
    ("src/ui.c", "    text_entry_set_blocks(false);",
     "    text_entry_set_blocks(enabled);",
     "the Entry setting leaks into the passphrase ring"),

    # T61: on the entropy screen each button does one thing.
    ("src/ui.c", "        if (events >= ENTROPY_TARGET_EVENTS) {", "        if (true) {",
     "NEXT proceeds before the pool is full"),
    ("src/ui.c",
     "        return;\n    }\n\n    /* UP and DOWN collect.",
     "    }\n\n    /* UP and DOWN collect.",
     "ACCEPT is a sample as well as the proceed button"),
    ("src/ui.c", '        oled_draw_string(7, 0, "MIX MIX BCK ----");',
     '        oled_draw_string(7, 0, "MIX MIX BCK MIX");',
     "the footer offers ACCEPT as a third way to collect"),

    # T39b: the fingerprint is what tells two passphrase wallets apart.
    ("src/ui.c",
     "            return;\n        }\n    }\n\n    wallet_info_refresh();\n}",
     "            return;\n        }\n    }\n}",
     "the wallet screen shows no fingerprint"),
    # Not a mutant of draw_master_xfp's own empty check: every caller now
    # tests master_xfp first and draws "No fingerprint" instead, so flipping
    # it is equivalent code. test_ui asserts no placeholder is ever drawn.
    ("src/ui.c", '    draw_master_xfp(3, "XFP");',
     "    /* mutant: no fingerprint */",
     "the passphrase confirmation drops the fingerprint"),
    ("src/ui.c", '    oled_draw_string_centered(2, line);\n    oled_draw_string(4, 0, "Typed on host!");',
     '    oled_draw_string(4, 0, "Typed on host!");',
     "the host passphrase confirmation drops the fingerprint"),

    # SLIP-39. Every one of these is a way to produce shares that look right
    # and are not recoverable by another implementation -- the failure mode a
    # backup format cannot have. The official vectors are what must notice.
    ("src/slip39-backup.c", "if (memcmp(check, digest, DIGEST_LEN) != 0) {",
     "if (false) {", "SLIP-39 digest check dropped"),
    ("src/slip39-backup.c", "if (rs1024(ext, w, words) != 1) {",
     "if (false) {", "SLIP-39 checksum not verified"),
    ("src/slip39-backup.c", "if ((w[4] >> (9 - i)) & 1) {",
     "if (false) {", "SLIP-39 nonzero padding accepted"),
    ("src/slip39-backup.c", "size_t pad = value_bits % 16;",
     "size_t pad = value_bits % 10;", "SLIP-39 padding length from the wrong modulus"),
    ("src/slip39-backup.c", "uint8_t ext = (uint8_t)((w[1] >> 4) & 1);",
     "uint8_t ext = (uint8_t)((w[1] >> 8) & 1);",
     "SLIP-39 extendable flag read from the wrong bit"),
    ("src/slip39-backup.c", "if (value_len < SLIP39_MIN_SECRET_LEN) {",
     "if (false) {", "SLIP-39 sub-128-bit share accepted"),
    ("src/slip39-backup.c", "if (member_idx[m] == table[i].member_index) {",
     "if (false) {", "SLIP-39 duplicate member index accepted"),
    ("src/slip39-backup.c", "if (members != threshold) {", "if (false) {",
     "SLIP-39 under-threshold group accepted"),
    ("src/slip39-backup.c", "if (groups_seen != sh->group_threshold) {",
     "if (groups_seen > sh->group_threshold) {",
     "SLIP-39 too few groups accepted"),
    ("src/slip39-backup.c", "if (table[i].id != sh->id ||", "if (false ||",
     "SLIP-39 shares from different backups mixed"),
    ("src/slip39-backup.c", "pass[0] = encrypt ? k : (uint8_t)(3 - k);",
     "pass[0] = k;", "SLIP-39 Feistel rounds not reversed on decrypt"),
    ("src/slip39-backup.c", "uint32_t iterations = 2500u << e;",
     "uint32_t iterations = 10000u << e;", "SLIP-39 PBKDF2 iteration count"),
    ("src/slip39-backup.c", "    memcpy(out, R, half);\n    memcpy(out + half, L, half);",
     "    memcpy(out, L, half);\n    memcpy(out + half, R, half);",
     "SLIP-39 Feistel halves swapped"),
    ("src/slip39-backup.c", "out->group_threshold    = (uint8_t)(((hdr >> 12) & 0xf) + 1);",
     "out->group_threshold    = (uint8_t)((hdr >> 12) & 0xf);",
     "SLIP-39 group threshold decoded without its +1"),
    ("src/slip39-backup.c", "            cmp = (w[len] == '\\0') ? 0 : 1;",
     "            cmp = 0;", "SLIP-39 word matched on a prefix"),
    ("src/slip39-backup.c", "    if (threshold == 1) {\n        for (uint8_t i = 0; i < count; i++) {",
     "    if (threshold == 0) {\n        for (uint8_t i = 0; i < count; i++) {",
     "SLIP-39 threshold-1 split takes the general path"),
    ("src/slip39-backup.c", "if (groups[i].threshold == 1 && groups[i].count > 1) {",
     "if (false) {", "SLIP-39 1-of-N group allowed"),
    ("src/slip39-backup.c", "    if (rng_hook) {", "    if (false) {",
     "SLIP-39 generation bypasses the entropy gate"),
    # AUDIT S8f: the slow half of "GEN" happens after the frame that announces
    # it, on the loop, not inside the button handler.
    ("src/ui.c", "                create_generate_pending = true;",
     "                /* mutant: generate inline */",
     "the deferred generation never runs"),
    ("src/ui.c",
     "    if (create_generate_pending && current_screen == SCREEN_WALLET_CREATE) {",
     "    if (create_generate_pending) {",
     "a generation request outlives the screen that made it"),

    # T42: a passphrase applied or dropped by another task must not leave a
    # fingerprint on screen that the device would not re-derive.
    ("src/ui.c",
     "    if (current_screen == SCREEN_WALLET_INFO &&\n        wallet_has_passphrase() != wallet_info_passphrase_shown) {",
     "    if (false) {",
     "the wallet screen keeps a fingerprint from a passphrase that is gone"),
    ("src/ui.c", '(unsigned)status.wallet_count,\n                 (unsigned)hd_account, wallet_has_passphrase() ? " P" : "");',
     '(unsigned)status.wallet_count,\n                 (unsigned)hd_account, "");',
     "nothing on screen says a passphrase is applied"),
    ("src/ui.c",
     "    lock_device();\n    ui_set_screen(SCREEN_PIN_UNLOCK);\n    return true;",
     "    pin_lock();\n    ui_set_screen(SCREEN_PIN_UNLOCK);\n    return true;",
     "auto-lock leaves the passphrase applied"),
    ("src/protocol.c", "        session_set_on_reset(host_passphrase_forget);",
     "        /* mutant: nothing to do when the session dies */",
     "a host passphrase outlives its session"),
    ("src/protocol.c",
     "void protocol_note_device_passphrase(void)\n{\n    host_passphrase_applied = false;\n}",
     "void protocol_note_device_passphrase(void)\n{\n}",
     "a device-typed passphrase is dropped when a host disconnects"),
    ("src/session.c", "    if (on_reset) {\n        on_reset();\n    }",
     "    /* mutant: nobody is told */",
     "session teardown notifies nothing"),

    # T45: the account level on the screens.
    ("src/ui.c", "    hd_account = account % HD_ACCOUNT_COUNT;",
     "    hd_account = account;",
     "the account selector runs past its bound"),
    ("src/ui.c", "    nvs_set_u8(nvs, UI_KEY_ACCOUNT, (uint8_t)hd_account);",
     "    nvs_set_u8(nvs, UI_KEY_ACCOUNT, 0);",
     "the account selection is not persisted"),
    ("src/ui.c",
     "             * account the previous owner left selected. */\n            hd_account_set(0);",
     "             * account the previous owner left selected. */",
     "a wipe leaves the previous owner's account selected"),
    # The reported bug and its neighbours: every lock path must lock the same
    # thing. Each mutant is one call site going back to closing the PIN gate
    # over an open vault.
    ("src/ui.c", "    pin_lock();\n    wallet_lock();",
     "    pin_lock();", "locking leaves the vault open behind the PIN gate"),
    ("src/ui.c", "             * that happens by itself. */\n            lock_device();",
     "             * that happens by itself. */\n            pin_lock();",
     "the menu's Lock device only closes the PIN gate"),
    ("src/ui.c",
     "                    lock_device();\n                    pending_mnemonic_display = true;",
     "                    pin_lock();\n                    pending_mnemonic_display = true;",
     "Show Seed re-asks for the PIN with the seed still in RAM"),
    ("src/ui.c", "    host_lock_pending = false;\n    lock_device();",
     "    host_lock_pending = false;\n    pin_lock();",
     "a host-requested lock only closes the PIN gate"),
    ("src/ui.c", "    memzero(master_xfp, sizeof(master_xfp));",
     "    /* mutant: the fingerprint outlives its seed */",
     "a fingerprint survives the lock that dropped its passphrase"),
]

BINARIES = ["test_protocol", "test_ble_chunk", "test_ui", "test_text_entry",
            "test_slip39"]

# Which suites even compile the mutated file. A suite that does not link it
# cannot notice the mutant, so building it proves nothing and costs a rebuild;
# anything not listed here (headers especially) falls back to all of them.
SUITES_FOR = {
    "src/text-entry.c": ["test_text_entry", "test_ui"],
    "src/ui.c":         ["test_ui"],
    "src/ble-chunk.c":  ["test_ble_chunk", "test_ui", "test_protocol"],
    "src/slip39-backup.c": ["test_slip39"],
    # session.c is linked by both endpoint suites; the protocol one is what
    # notices a teardown that forgets to tell anyone (T42).
    "src/session.c":    ["test_protocol", "test_ui"],
}


def run(cmd, **kw):
    # A mutant that sends a suite into an infinite loop is caught, not tolerated:
    # without a deadline the whole run stalls on it and reports nothing.
    try:
        return subprocess.run(cmd, shell=True, cwd=ROOT, capture_output=True,
                              text=True, timeout=120, **kw)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, returncode=124)


def build_and_run(binaries=None):
    """Return the set of suites that failed (build failure counts as caught)."""
    failed = set()
    for b in (binaries if binaries is not None else BINARIES):
        r = run(f"make -C sim build/{b}")
        if r.returncode != 0:
            failed.add(b + " (build)")
            continue
        r = run(f"./sim/build/{b}")
        if r.returncode != 0:
            failed.add(b)
    return failed


def main():
    # An optional substring selects a subset, for iterating on one change
    # without paying for the whole matrix. A bare run is still the whole thing.
    only = sys.argv[1] if len(sys.argv) > 1 else None

    baseline = build_and_run()
    if baseline:
        print("baseline is not green:", baseline)
        return 1

    selected = [m for m in MUTANTS if only is None or only in m[3] or only in m[0]]

    killed, survived = 0, []
    for path, find, repl, label in selected:
        full = os.path.join(ROOT, path)
        original = open(full).read()
        if find not in original:
            print(f"SKIP  {label}: pattern not found in {path}")
            survived.append(label + " (pattern missing)")
            continue
        open(full, "w").write(original.replace(find, repl, 1))
        try:
            failed = build_and_run(SUITES_FOR.get(path))
        finally:
            open(full, "w").write(original)
        if failed:
            killed += 1
            print(f"KILLED  {label}  <- {', '.join(sorted(failed))}")
        else:
            survived.append(label)
            print(f"SURVIVED {label}")

    # Leave the tree rebuilt from pristine sources.
    build_and_run()

    print(f"\n{killed}/{len(selected)} mutants killed")
    for s in survived:
        print("  survived:", s)
    return 0 if not survived else 1


if __name__ == "__main__":
    sys.exit(main())
