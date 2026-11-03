// REFERENCE IMPLEMENTATION, NOT PART OF THE SDK BUILD, AND NOT COMPILED: no
// Swift toolchain on the build machine. A starting point for an iOS or
// macOS app's quick unlock: copy it into the app, review it, build it with
// the generated LeekZcash module, test it on real devices.
//
// The Storage Key is wrapped with ECIES (SecKeyCreateEncryptedData,
// eciesEncryptionCofactorVariableIVX963SHA256AESGCM) to a P-256 key
// generated in the Secure Enclave. Wrapping needs only the public key;
// unwrapping makes the Secure Enclave use the private key, which its access
// control allows only after Face ID / Touch ID or the device passcode
// (.userPresence), on this device only, while a passcode is set. The
// passcode is checked by the system with its own attempt limits; nothing
// here derives a key from it.

import CryptoKit
import Foundation
import LocalAuthentication
import Security

/// Quick unlock with the Secure Enclave. One instance per app, network and
/// account: `tag` names the key, for example
/// `"xyz.leekwallet.zcash.quick-unlock.main.0"`. Call the SDK from a
/// background task: `unwrap` blocks while the system prompt is up.
final class SecureEnclaveWrapper: KeyWrapper {
    private let tag: Data
    private let reason: String
    private let algorithm = SecKeyAlgorithm.eciesEncryptionCofactorVariableIVX963SHA256AESGCM

    init(tag: String, reason: String = "Unlock your wallet") {
        self.tag = Data(tag.utf8)
        self.reason = reason
    }

    func info() -> KeyWrapperInfo {
        // No Secure Enclave (Simulator, old Macs) or no passcode: not offered.
        let hasPasscode = LAContext().canEvaluatePolicy(.deviceOwnerAuthentication, error: nil)
        let ok = SecureEnclave.isAvailable && hasPasscode && (try? privateKey(create: true)) != nil
        return KeyWrapperInfo(
            hardwareBacked: ok,
            userAuthRequired: ok,
            label: ok ? "Face ID, Touch ID or passcode (Secure Enclave)" : "Not available"
        )
    }

    func wrap(key: Data) throws -> Data {
        var input = key
        defer { input.resetBytes(in: 0..<input.count) }
        guard let pub = SecKeyCopyPublicKey(try privateKey(create: true)) else {
            throw KeyWrapperFailure.Failed
        }
        var error: Unmanaged<CFError>?
        guard let blob = SecKeyCreateEncryptedData(pub, algorithm, input as CFData, &error) as Data? else {
            throw KeyWrapperFailure.Failed
        }
        return blob // ephemeral public key (65) || ciphertext (32) || tag (16)
    }

    func unwrap(blob: Data) throws -> Data {
        let context = LAContext()
        context.localizedReason = reason
        guard let key = try privateKey(create: false, context: context) else {
            throw KeyWrapperFailure.Invalidated
        }
        var error: Unmanaged<CFError>?
        guard let plain = SecKeyCreateDecryptedData(key, algorithm, blob as CFData, &error) as Data? else {
            let e = error?.takeRetainedValue()
            switch e.map({ CFErrorGetCode($0) }) {
            case Int(errSecUserCanceled), Int(errSecAuthFailed), LAError.userCancel.rawValue,
                 LAError.appCancel.rawValue, LAError.systemCancel.rawValue:
                throw KeyWrapperFailure.Cancelled
            default:
                // Another key, or a blob that does not authenticate.
                throw KeyWrapperFailure.Invalidated
            }
        }
        return plain
    }

    func deleteKey() throws {
        let query: [String: Any] = [
            kSecClass as String: kSecClassKey,
            kSecAttrApplicationTag as String: tag,
        ]
        let status = SecItemDelete(query as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else {
            throw KeyWrapperFailure.Failed
        }
    }

    /// The Secure Enclave key; created on first use if `create`.
    private func privateKey(create: Bool, context: LAContext? = nil) throws -> SecKey? {
        var query: [String: Any] = [
            kSecClass as String: kSecClassKey,
            kSecAttrApplicationTag as String: tag,
            kSecAttrKeyType as String: kSecAttrKeyTypeECSECPrimeRandom,
            kSecReturnRef as String: true,
        ]
        if let context { query[kSecUseAuthenticationContext as String] = context }
        var item: CFTypeRef?
        if SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess {
            return (item as! SecKey)
        }
        guard create else { return nil }
        var error: Unmanaged<CFError>?
        guard let access = SecAccessControlCreateWithFlags(
            nil,
            kSecAttrAccessibleWhenPasscodeSetThisDeviceOnly,
            // .userPresence: biometry or the passcode. An app that wants the
            // key dropped when a new finger or face is enrolled uses
            // [.privateKeyUsage, .biometryCurrentSet] (no passcode fallback).
            [.privateKeyUsage, .userPresence],
            &error
        ) else {
            throw KeyWrapperFailure.Failed
        }
        let attributes: [String: Any] = [
            kSecAttrKeyType as String: kSecAttrKeyTypeECSECPrimeRandom,
            kSecAttrKeySizeInBits as String: 256,
            kSecAttrTokenID as String: kSecAttrTokenIDSecureEnclave,
            kSecPrivateKeyAttrs as String: [
                kSecAttrIsPermanent as String: true,
                kSecAttrApplicationTag as String: tag,
                kSecAttrAccessControl as String: access,
            ],
        ]
        guard let key = SecKeyCreateRandomKey(attributes as CFDictionary, &error) else {
            throw KeyWrapperFailure.Failed
        }
        return key
    }
}
