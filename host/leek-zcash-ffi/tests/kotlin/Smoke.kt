// Smoke test of the generated Kotlin bindings on the JVM (JNA loads
// libleek_zcash_ffi from jna.library.path). No board: the byte API, the
// error mapping, the account record, quick unlock through a Kotlin
// KeyWrapper, and a Kotlin FrameTransport whose
// failures and a cancel from another thread come back typed.
// Run with scripts/kotlin-smoke.sh.

import io.leekwallet.zcash.sdk.*
import java.io.File

fun check(cond: Boolean, what: String) {
    if (!cond) throw AssertionError(what)
}

class Refused : FrameTransport {
    override fun send(bytes: ByteArray) = throw TransportFailure.PermissionDenied()
    override fun recv(timeoutMs: UInt): ByteArray = throw TransportFailure.PermissionDenied()
}

class Throws : FrameTransport {
    override fun send(bytes: ByteArray) = throw IllegalStateException("usb gone")
    override fun recv(timeoutMs: UInt): ByteArray = throw IllegalStateException("usb gone")
}

class Silent : FrameTransport {
    override fun send(bytes: ByteArray) {}
    override fun recv(timeoutMs: UInt): ByteArray {
        Thread.sleep(timeoutMs.toLong())
        return ByteArray(0)
    }
}

// A stand-in platform key (XOR with a pad): only the contract is tested.
class FakeWrapper(private val hardware: Boolean) : KeyWrapper {
    var cancel = false
    var deleted = false
    override fun info() = KeyWrapperInfo(hardware, true, "Fingerprint (smoke)")
    override fun wrap(key: ByteArray) = ByteArray(key.size) { (key[it].toInt() xor 0x5a).toByte() }
    override fun unwrap(blob: ByteArray): ByteArray {
        if (cancel) throw KeyWrapperFailure.Cancelled()
        if (deleted) throw KeyWrapperFailure.Invalidated()
        return ByteArray(blob.size) { (blob[it].toInt() xor 0x5a).toByte() }
    }
    override fun deleteKey() {
        deleted = true
    }
}

object NoCode : PairingListener {
    override fun onPairingCode(code: String) {}
}

fun hex(s: String) = ByteArray(s.length / 2) { s.substring(2 * it, 2 * it + 2).toInt(16).toByte() }

fun main(args: Array<String>) {
    val vectors = File(args[0])
    val pczt = File(vectors, "pczt/iw_send_shielded.pczt").readBytes()
    val keys = File(vectors, "keys.json").readText()
    // The test/abandon-art/pp0/acct0 entry, without a JSON library.
    val entry = keys.substring(keys.indexOf("\"test/abandon-art/pp0/acct0\""))
    fun field(name: String) = Regex("\"$name\"\\s*:\\s*\"([^\"]+)\"").find(entry)!!.groupValues[1]
    val account = AccountMeta(hex(field("seed_fingerprint")), 0u, Network.TEST, field("ufvk"))

    // Byte API.
    val full = signerViewBytes(pczt, SignerView.FULL)
    val compact = signerViewBytes(pczt, SignerView.COMPACT)
    check(compact.size < full.size, "compact view is smaller")
    preflight(pczt, account, null)
    val req = prepareSdSign(pczt, account, SignerView.FULL)
    check(req.fileName == "LEEKZ/unsigned.pczt" && req.zsigFileName == "LEEKZ/signed.zsig", "sd paths")
    check(req.bytes.contentEquals(full) && req.sentHash.size == 32, "sd request")

    // The call order for a PCZT from the wallet's own builder (Zodl:
    // redactPcztForSigner): prepareForSigner, a no-op when the derivations
    // are there, then preflight (signPczt runs it too).
    preflight(prepareForSigner(pczt, account.ufvk!!, account.seedFingerprint, 0u), account, null)

    // What the JNI library offered: boardUpdateText(), firmwareOutdated(level).
    check(boardUpdateText().startsWith("Update your board"), "board update text")
    check(firmwareOutdated(null) && firmwareOutdated(1u), "no level, or level 1, is outdated")
    check(!firmwareOutdated(minFirmwareCompat()), "the current level is not")

    // Errors: category, stable code, recovery, and the sentence to show.
    try {
        signerViewBytes("nope".toByteArray(), SignerView.FULL)
        check(false, "a bad PCZT is refused")
    } catch (e: LeekException.Pczt) {
        check(e.code.toInt() == 512 && e.recovery == Recovery.DO_NOT_RETRY, "parse error: $e")
        check(e.reason == "the transaction does not parse", "reason: ${e.reason}")
    }
    try {
        applyZsig(pczt, full, "ZSIG".toByteArray())
        check(false, "a bad .zsig is refused")
    } catch (e: LeekException.Signature) {
        check(e.code.toInt() == 601, "zsig error: $e")
    }

    // Account record.
    val rec = AccountRecord(account.seedFingerprint, listOf(AccountEntry(account.ufvk!!, 0u, "Board", 3_000_000u)))
    val back = decodeAccountRecord(encodeAccountRecord(rec))
    check(back.accounts == rec.accounts && back.seedFingerprint.contentEquals(rec.seedFingerprint), "record")
    check(accountRecordType() == "leek-zcash-accounts", "record type")

    // The View key QR, out and back through the SDK's animated-QR pieces.
    QrEncoder.accountRecord(rec, qrDefaults().fragmentLen).use { enc ->
        check(enc.frame(0uL).startsWith("UR:LEEK-ZCASH-ACCOUNTS/"), "qr frame")
        QrScanner(QrScanKind.ACCOUNT_RECORD, null).use { scan ->
            var i = 0uL
            while (!scan.isComplete() && i < 300uL) { scan.feed(enc.frame(i)); i++ }
            val got = scan.accountRecord()
            check(got != null && got.accounts == rec.accounts, "qr record")
        }
    }

    // How to show the signing QR: the phone's rules and tips.
    val hints = qrDisplayHints()
    check(hints.phoneFragmentLen == 25u && hints.phoneFrameMs == 350u && hints.quietZoneModules == 2u, "display hints")
    check(hints.maxBrightness && hints.keepScreenOn && hints.errorCorrection == "L", "display rules")
    check(hints.phoneTips.first().contains("anti-flicker") && qrDefaults().fragmentChoices.contains(25u), "display tips")

    // Stored app data: the draft's vector 1 key, sealed and opened.
    val sk = StorageKey.fromBytes(
        hex("f43c678ee392cc4552daa3bde21c3cb15b416811dfdacf64e7aceca6114bd0d0"),
        "xyz.leekwallet.zcash", 0u, Network.MAIN,
    )
    check(sk.dbKey().contentEquals(hex("abd9811ecdfd468b538e7f11714bdacca99e354ea622d31f0ee3b2cee3f85973")), "db subkey")
    check(sk.openUfvk(sk.sealUfvk("uview1example")) == "uview1example", "ufvk round trip")

    // Quick unlock through a Kotlin KeyWrapper.
    val sealedDb = sk.sealDb("wallet".toByteArray())
    val wrapper = FakeWrapper(true)
    val record = QuickUnlock.create(sk, wrapper).toBytes()
    val qu = QuickUnlock.fromBytes(record)
    check(qu.appId() == "xyz.leekwallet.zcash" && qu.label() == "Fingerprint (smoke)", "record fields")
    val opened = qu.open("xyz.leekwallet.zcash", 0u, Network.MAIN, wrapper)
    check(String(opened.openDb(sealedDb)) == "wallet", "quick unlock opens the data")
    opened.forget()
    try {
        QuickUnlock.create(sk, FakeWrapper(false))
        check(false, "a software key is refused")
    } catch (e: LeekException.Storage) {
        check(e.code.toInt() == 706, "insecure wrapper: $e")
    }
    wrapper.cancel = true
    try {
        qu.open("xyz.leekwallet.zcash", 0u, Network.MAIN, wrapper)
        check(false, "cancelled")
    } catch (e: LeekException.Storage) {
        check(e.code.toInt() == 708 && e.recovery == Recovery.RETRY_SAME_LINK, "cancel: $e")
    }
    wrapper.cancel = false
    disableQuickUnlock(wrapper)
    try {
        qu.open("xyz.leekwallet.zcash", 0u, Network.MAIN, wrapper)
        check(false, "disabled")
    } catch (e: LeekException.Storage) {
        check(e.code.toInt() == 707, "disabled: $e")
    }
    qu.close()
    sk.forget()
    try {
        sk.openDb(ByteArray(0))
        check(false, "a forgotten key is refused")
    } catch (e: LeekException.Storage) {
        check(e.code.toInt() == 704, "forgotten: $e")
    }
    sk.close()

    // A Kotlin transport: its failures keep their kind.
    LeekDevice(Refused(), LinkKind.STREAM).use { d ->
        try {
            d.pair(NoCode)
            check(false, "refused")
        } catch (e: LeekException.Link) {
            check(e.code.toInt() == 101 && e.recovery == Recovery.RECONNECT, "permission: $e")
        }
    }
    LeekDevice(Throws(), LinkKind.FRAMES).use { d ->
        // Not paired yet: refused before the transport is used.
        try {
            d.ensureUnlocked()
            check(false, "unpaired")
        } catch (e: LeekException.Session) {
            check(e.code.toInt() == 201 && e.recovery == Recovery.RECONNECT, "unpaired: $e")
        }
        try {
            d.pair(NoCode)
            check(false, "thrown")
        } catch (e: LeekException.Link) {
            check(e.code.toInt() == 102, "unexpected exception is a dropped link: $e")
        }
    }
    // Cancelled from another thread while the board is silent.
    LeekDevice(Silent(), LinkKind.FRAMES).use { d ->
        val t = Thread { Thread.sleep(300); d.cancel() }
        t.start()
        val started = System.nanoTime()
        try {
            d.pair(NoCode)
            check(false, "cancelled")
        } catch (e: LeekException.Link) {
            check(e.code.toInt() == 104, "cancel: $e")
        }
        t.join()
        check((System.nanoTime() - started) / 1_000_000 < 3000, "cancel is prompt")
    }
    println("kotlin smoke: ok")
}
