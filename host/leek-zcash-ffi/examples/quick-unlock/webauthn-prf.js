// REFERENCE SNIPPET, NOT PART OF ANY BUILD, NOT RUN: quick unlock for the
// Chrome wallet with the WebAuthn PRF extension (Chrome 116+ with a platform
// authenticator such as Windows Hello, Touch ID or Android, or a security
// key that supports hmac-secret).
//
// The authenticator holds a per-credential secret and returns
// HMAC(secret, salt) only after user verification (biometric or the
// authenticator's PIN, with its own attempt limits). The PRF output is fed
// through HKDF into an AES-256-GCM key that wraps the Storage Key. A stolen
// disk holds the wrapped key and the salt, never the PRF output.
//
// There are no browser bindings of leek-zcash yet, so this is the KeyWrapper
// contract in async JavaScript (info / wrap / unwrap / deleteKey). Until the
// SDK has a WebAssembly build, the wallet stores what wrap() returns
// together with app_id, coin type and account, and checks the unwrapped key
// before using it (open a sealed object with it: a wrong key fails the key
// check). Offer quick unlock only when info() says both flags are true.
//
// Caveat: a passkey synced by a password manager (Google Password Manager,
// iCloud Keychain) is not bound to this device; its PRF secret follows the
// user's account. Say so, or ask for a device-bound authenticator.

const RP_ID = location.hostname;
const INFO = new TextEncoder().encode('LeekWallet quick unlock v1');

export class WebAuthnPrfWrapper {
  // credentialId: Uint8Array from an earlier create(), or null.
  constructor(credentialId, prfSalt) {
    this.credentialId = credentialId;
    this.prfSalt = prfSalt; // 32 random bytes, stored with the record
  }

  // Registers a credential with PRF and user verification required.
  static async create(userName) {
    const cred = await navigator.credentials.create({
      publicKey: {
        rp: { id: RP_ID, name: 'LeekWallet' },
        user: { id: crypto.getRandomValues(new Uint8Array(16)), name: userName, displayName: userName },
        challenge: crypto.getRandomValues(new Uint8Array(32)),
        pubKeyCredParams: [{ type: 'public-key', alg: -7 }, { type: 'public-key', alg: -257 }],
        authenticatorSelection: { userVerification: 'required', residentKey: 'discouraged' },
        extensions: { prf: {} },
      },
    });
    const enabled = cred.getClientExtensionResults().prf?.enabled === true;
    const w = new WebAuthnPrfWrapper(new Uint8Array(cred.rawId), crypto.getRandomValues(new Uint8Array(32)));
    w.prfEnabled = enabled;
    return w;
  }

  info() {
    // The authenticator keeps the PRF secret in hardware and asks for user
    // verification; without PRF there is nothing to offer.
    const ok = this.prfEnabled === true;
    return { hardwareBacked: ok, userAuthRequired: ok, label: ok ? 'Passkey (biometric or PIN)' : 'Not available' };
  }

  async #aesKey() {
    const assertion = await navigator.credentials.get({
      publicKey: {
        rpId: RP_ID,
        challenge: crypto.getRandomValues(new Uint8Array(32)),
        allowCredentials: [{ type: 'public-key', id: this.credentialId }],
        userVerification: 'required',
        extensions: { prf: { eval: { first: this.prfSalt } } },
      },
    });
    // NotAllowedError above: cancelled or timed out (code 708 in the SDK).
    const first = assertion.getClientExtensionResults().prf?.results?.first;
    if (!first) throw new Error('Invalidated');
    const ikm = await crypto.subtle.importKey('raw', first, 'HKDF', false, ['deriveKey']);
    return crypto.subtle.deriveKey(
      { name: 'HKDF', hash: 'SHA-256', salt: new Uint8Array(0), info: INFO },
      ikm, { name: 'AES-GCM', length: 256 }, false, ['encrypt', 'decrypt'],
    );
  }

  // key: the 32-byte Storage Key. Returns iv (12) || ciphertext || tag (16).
  async wrap(key) {
    const k = await this.#aesKey();
    const iv = crypto.getRandomValues(new Uint8Array(12));
    const ct = new Uint8Array(await crypto.subtle.encrypt({ name: 'AES-GCM', iv }, k, key));
    key.fill(0);
    const out = new Uint8Array(12 + ct.length);
    out.set(iv);
    out.set(ct, 12);
    return out;
  }

  async unwrap(blob) {
    const k = await this.#aesKey();
    try {
      return new Uint8Array(await crypto.subtle.decrypt({ name: 'AES-GCM', iv: blob.slice(0, 12) }, k, blob.slice(12)));
    } catch {
      throw new Error('Invalidated'); // another credential, or a changed blob
    }
  }

  // A web page cannot delete a passkey: forget the credential id and the
  // record, and tell the user they may remove the passkey in their settings.
  async deleteKey() {
    this.credentialId = null;
  }
}
