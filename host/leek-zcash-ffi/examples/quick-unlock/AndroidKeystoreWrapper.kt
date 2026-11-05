// REFERENCE IMPLEMENTATION, NOT PART OF THE SDK BUILD. A starting point for
// an Android app's quick unlock: copy it into the app, review it, test it on
// real devices (StrongBox and TEE-only ones, with and without a biometric).
// Compiled against android-34, androidx.biometric 1.4.0-alpha05 and the
// generated bindings (see the FFI README); never run here.
//
// The Storage Key is wrapped with AES-256-GCM under a key generated in the
// Android Keystore (StrongBox when the device has it, otherwise the TEE)
// that the hardware uses only after a fresh biometric or device PIN for each
// operation (BiometricPrompt with a CryptoObject). The key is invalidated
// when a new biometric is enrolled or the lock screen is removed; the app
// then gets code 707 and unlocks with the board. Nothing here derives a key
// from a PIN: the PIN is checked by the lock screen and the hardware, with
// their attempt limits.

package io.leekwallet.zcash.examples

import android.content.pm.PackageManager
import android.os.Build
import android.os.Looper
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyInfo
import android.security.keystore.KeyPermanentlyInvalidatedException
import android.security.keystore.KeyProperties
import android.security.keystore.StrongBoxUnavailableException
import androidx.biometric.BiometricManager
import androidx.biometric.BiometricManager.Authenticators.BIOMETRIC_STRONG
import androidx.biometric.BiometricManager.Authenticators.DEVICE_CREDENTIAL
import androidx.biometric.BiometricPrompt
import androidx.core.content.ContextCompat
import androidx.fragment.app.FragmentActivity
import io.leekwallet.zcash.sdk.KeyWrapper
import io.leekwallet.zcash.sdk.KeyWrapperFailure
import io.leekwallet.zcash.sdk.KeyWrapperInfo
import java.security.KeyStore
import java.security.UnrecoverableKeyException
import java.util.concurrent.CountDownLatch
import javax.crypto.AEADBadTagException
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.SecretKeyFactory
import javax.crypto.spec.GCMParameterSpec

/**
 * Quick unlock with the Android Keystore. One instance per app, network and
 * account: [alias] names the Keystore key, for example
 * `"leek-quick-unlock/main/0"`. Call the SDK (`QuickUnlock.create`,
 * `QuickUnlock.open`) from a background thread: [wrap] and [unwrap] show
 * BiometricPrompt on the main thread and wait for it.
 */
class AndroidKeystoreWrapper(
    private val activity: FragmentActivity,
    private val alias: String,
    private val title: String = "Unlock your wallet",
) : KeyWrapper {
    private val keyStore = KeyStore.getInstance(KEYSTORE).apply { load(null) }

    // Device credential with a CryptoObject needs API 30; before that the
    // key is biometric-only.
    private val authenticators =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) BIOMETRIC_STRONG or DEVICE_CREDENTIAL else BIOMETRIC_STRONG

    /** Reports the key as the Keystore describes it, creating it if needed. */
    override fun info(): KeyWrapperInfo {
        val usable = BiometricManager.from(activity).canAuthenticate(authenticators) == BiometricManager.BIOMETRIC_SUCCESS
        val key = if (usable) runCatching { key() }.getOrNull() else null
        val keyInfo = key?.let {
            SecretKeyFactory.getInstance(it.algorithm, KEYSTORE).getKeySpec(it, KeyInfo::class.java) as KeyInfo
        }
        val level = keyInfo?.let(::securityLevel)
        return KeyWrapperInfo(
            hardwareBacked = level != null,
            userAuthRequired = keyInfo?.isUserAuthenticationRequired == true,
            label = if (level == null) "Not available" else "Fingerprint, face or PIN ($level)",
        )
    }

    override fun wrap(key: ByteArray): ByteArray {
        val cipher = Cipher.getInstance(TRANSFORMATION)
        try {
            cipher.init(Cipher.ENCRYPT_MODE, key())
        } catch (e: KeyPermanentlyInvalidatedException) {
            // A stale key from before a new enrolment: start over.
            keyStore.deleteEntry(alias)
            cipher.init(Cipher.ENCRYPT_MODE, key())
        }
        val ready = authenticate(cipher)
        val sealed = try {
            ready.doFinal(key)
        } catch (e: Exception) {
            throw KeyWrapperFailure.Failed()
        } finally {
            key.fill(0)
        }
        // iv (12) || ciphertext (32) || tag (16)
        return ready.iv + sealed
    }

    override fun unwrap(blob: ByteArray): ByteArray {
        if (blob.size != IV_LEN + 32 + 16) throw KeyWrapperFailure.Invalidated()
        val existing = keyStore.getKey(alias, null) as? SecretKey ?: throw KeyWrapperFailure.Invalidated()
        val cipher = Cipher.getInstance(TRANSFORMATION)
        try {
            cipher.init(Cipher.DECRYPT_MODE, existing, GCMParameterSpec(128, blob, 0, IV_LEN))
        } catch (e: KeyPermanentlyInvalidatedException) {
            throw KeyWrapperFailure.Invalidated()
        } catch (e: UnrecoverableKeyException) {
            throw KeyWrapperFailure.Invalidated()
        }
        val ready = authenticate(cipher)
        return try {
            ready.doFinal(blob, IV_LEN, blob.size - IV_LEN)
        } catch (e: AEADBadTagException) {
            throw KeyWrapperFailure.Invalidated()
        } catch (e: Exception) {
            throw KeyWrapperFailure.Failed()
        }
    }

    override fun deleteKey() {
        try {
            keyStore.deleteEntry(alias)
        } catch (e: Exception) {
            throw KeyWrapperFailure.Failed()
        }
    }

    /** The Keystore key, generated on first use: StrongBox if present, else the TEE. */
    private fun key(): SecretKey {
        (keyStore.getKey(alias, null) as? SecretKey)?.let { return it }
        val strongBox = Build.VERSION.SDK_INT >= Build.VERSION_CODES.P &&
            activity.packageManager.hasSystemFeature(PackageManager.FEATURE_STRONGBOX_KEYSTORE)
        return try {
            generate(strongBox)
        } catch (e: StrongBoxUnavailableException) {
            generate(false)
        }
    }

    private fun generate(strongBox: Boolean): SecretKey {
        val spec = KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setKeySize(256)
            .setRandomizedEncryptionRequired(true)
            // Every use needs a fresh authentication, bound to this operation
            // through the CryptoObject; no validity window.
            .setUserAuthenticationRequired(true)
            .setInvalidatedByBiometricEnrollment(true)
            .apply {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    setUserAuthenticationParameters(
                        0,
                        KeyProperties.AUTH_BIOMETRIC_STRONG or KeyProperties.AUTH_DEVICE_CREDENTIAL,
                    )
                } else {
                    @Suppress("DEPRECATION")
                    setUserAuthenticationValidityDurationSeconds(-1)
                }
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                    setIsStrongBoxBacked(strongBox)
                    setUnlockedDeviceRequired(true)
                }
            }
            .build()
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE).run {
            init(spec)
            generateKey()
        }
    }

    /** Shows BiometricPrompt for [cipher] and blocks this (background) thread until it ends. */
    private fun authenticate(cipher: Cipher): Cipher {
        check(Looper.myLooper() != Looper.getMainLooper()) { "call QuickUnlock off the main thread" }
        val done = CountDownLatch(1)
        var result: Cipher? = null
        var failure: KeyWrapperFailure = KeyWrapperFailure.Cancelled()
        val executor = ContextCompat.getMainExecutor(activity)
        executor.execute {
            val callback = object : BiometricPrompt.AuthenticationCallback() {
                override fun onAuthenticationSucceeded(r: BiometricPrompt.AuthenticationResult) {
                    result = r.cryptoObject?.cipher
                    done.countDown()
                }

                override fun onAuthenticationError(code: Int, message: CharSequence) {
                    failure = when (code) {
                        BiometricPrompt.ERROR_USER_CANCELED, BiometricPrompt.ERROR_NEGATIVE_BUTTON,
                        BiometricPrompt.ERROR_CANCELED, BiometricPrompt.ERROR_TIMEOUT,
                        // Too many attempts: the hardware's limit, not ours.
                        BiometricPrompt.ERROR_LOCKOUT, BiometricPrompt.ERROR_LOCKOUT_PERMANENT,
                        -> KeyWrapperFailure.Cancelled()
                        else -> KeyWrapperFailure.Failed()
                    }
                    done.countDown()
                }
                // onAuthenticationFailed (a finger that does not match) keeps
                // the prompt up; the hardware counts the attempts.
            }
            val info = BiometricPrompt.PromptInfo.Builder()
                .setTitle(title)
                .setAllowedAuthenticators(authenticators)
                .apply { if (authenticators == BIOMETRIC_STRONG) setNegativeButtonText("Use the board") }
                .build()
            BiometricPrompt(activity, executor, callback).authenticate(info, BiometricPrompt.CryptoObject(cipher))
        }
        done.await()
        return result ?: throw failure
    }

    private companion object {
        const val KEYSTORE = "AndroidKeyStore"
        const val TRANSFORMATION = "AES/GCM/NoPadding"
        const val IV_LEN = 12

        /** "StrongBox" or "TEE"; null if the key is not in secure hardware. */
        fun securityLevel(info: KeyInfo): String? =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                when (info.securityLevel) {
                    KeyProperties.SECURITY_LEVEL_STRONGBOX -> "StrongBox"
                    KeyProperties.SECURITY_LEVEL_TRUSTED_ENVIRONMENT -> "TEE"
                    KeyProperties.SECURITY_LEVEL_UNKNOWN_SECURE -> "secure hardware"
                    else -> null
                }
            } else {
                @Suppress("DEPRECATION")
                if (info.isInsideSecureHardware) "TEE" else null
            }
    }
}
