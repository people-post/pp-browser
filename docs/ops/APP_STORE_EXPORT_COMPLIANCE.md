# App Store export compliance (encryption)

**Tier:** ops / howto

Checklist for answering Apple App Store Connect / TestFlight **export compliance** questions when shipping an iOS (or Mac App Store) build. Normative crypto specs live in contracts; this doc is the **submission cheat sheet**.

**Related:** [MESSAGE_ENCRYPTION.md](../contracts/MESSAGE_ENCRYPTION.md), [AT_REST_ENCRYPTION.md](../contracts/AT_REST_ENCRYPTION.md), [IOS_BUILD.md](IOS_BUILD.md), [MACOS_SIGNING.md](MACOS_SIGNING.md).

**Not legal advice.** Confirm with counsel before filing; EAR and Apple forms change.

---

## Short answer for this product

PP ships **non–Apple-OS encryption** (libsodium AEAD, PIN vault, PQ KEM/signatures, libp2p TLS stacks). For App Store Connect:

| Question theme | Expected answer |
|----------------|-----------------|
| Does the app use encryption? | **Yes** |
| Limited to encryption in the Apple OS only? | **No** |
| Proprietary / unpublished algorithms (not accepted by IEEE, IETF, ITU, etc.)? | **No** (see inventory — industry-standard / NIST-published) |
| Available on the App Store in **France**? | **Yes** → upload French encryption declaration; **No** → French form not required by Apple |

Apple’s documentation matrix ([Export compliance documentation for encryption](https://developer.apple.com/help/app-store-connect/reference/export-compliance-documentation-for-encryption)):

| Encryption in use | Docs Apple asks for |
|-------------------|---------------------|
| Apple OS only | None |
| Industry standard, **not** provided by Apple OS | French declaration **if** distributing in France |
| Proprietary / unpublished | US **CCATS** + French declaration (France) |

For current PP crypto, plan on **French declaration when France is enabled**, and usually **not** a US CCATS — unless Apple’s questionnaire or counsel classifies something as proprietary.

---

## Crypto inventory (copy into forms / counsel notes)

License: **MIT** (`LICENSE`). Source is intended to be **publicly available** open source (same tree as this repo).

### Message E2E (P2P chat)

| Role | Algorithm | Key / size | Library / API |
|------|-----------|------------|---------------|
| Message body AEAD | XChaCha20-Poly1305 (IETF) | 256-bit key; 24-byte nonce | libsodium `crypto_aead_xchacha20poly1305_ietf_*` |
| Session key derivation | HKDF-SHA256 | 256-bit output | libsodium |
| Master PSK | CSPRNG | 32 bytes | libsodium `randombytes_buf` |
| PSK fingerprint | BLAKE2b-256 | 32-byte digest | libsodium |
| Relay envelope signature (legacy / transitional) | Ed25519 | 32-byte pk / 64-byte sig | OpenSSL EVP |
| Public-tier auto-key (`e2e_public`) | ML-KEM-768 | FIPS 203; pk 1184 B, ct 1088 B, sk 2400 B | vendored `mlkem-native` |
| Account / device signing (PQ path) | ML-DSA-65 | FIPS 204 | vendored `mldsa-native` |

Normative detail: [MESSAGE_ENCRYPTION.md](../contracts/MESSAGE_ENCRYPTION.md). Code: `src/base/crypto/` (`MessageCipher`, `SessionKeyDeriver`, `HybridKem`, `MlDsa`, …).

### At-rest (PIN vault / DEK)

| Role | Algorithm | Key / size | Library / API |
|------|-----------|------------|---------------|
| PIN → KEK | Argon2id | interactive `crypto_pwhash` limits stored in `vault.bin` | libsodium |
| Wrap DEK / file AEAD | XChaCha20-Poly1305 | 32-byte DEK | libsodium |
| Transcript bodies (when vault present) | XChaCha20-Poly1305 under DEK | same | libsodium |

Normative detail: [AT_REST_ENCRYPTION.md](../contracts/AT_REST_ENCRYPTION.md).

### Transport / platform crypto (also in the binary)

| Role | Typical algorithms | Notes |
|------|--------------------|--------|
| HTTPS / TLS (curl, etc.) | Standard TLS cipher suites | May use system or vendored TLS |
| libp2p Noise / TLS | Standard Noise/TLS constructions | In-tree libp2p + BoringSSL / deps under `third_party/` |

These are still **industry-standard** crypto outside (or in addition to) Apple’s OS APIs — keep **“Apple OS only?” = No**.

### What we are **not** claiming as proprietary crypto

- Custom **wire layouts** (AAD, domain-separated sign bytes, `ChatPayload`) are **published** in-repo contracts; they compose standard primitives.
- Algorithms above are published standards (IETF RFCs, PHC/Argon2, NIST FIPS 203/204, etc.), not secret ciphers.

If crypto functionality changes (new AEAD, new KEM, closed-source-only binary), **update this inventory** and re-check App Store answers.

---

## App Store Connect workflow

1. Fill **App Description** and set **availability** (including whether France is on) before uploading encryption docs — Apple uses that to review.
2. Open the build → **Provide Export Compliance Information**, or **App Encryption Documentation**.
3. Answer using the table in [Short answer](#short-answer-for-this-product) and the [inventory](#crypto-inventory-copy-into-forms--counsel-notes).
4. If docs are required, upload under **App Encryption Documentation** (French form when France is available).
5. After Apple approves (~2 business days if complete), copy the issued code into Info.plist:

```xml
<key>ITSAppUsesNonExemptEncryption</key>
<true/>
<key>ITSEncryptionExportComplianceCode</key>
<string>PASTE_CODE_FROM_APP_STORE_CONNECT</string>
```

`packaging/ios/Info.plist` already sets `ITSAppUsesNonExemptEncryption` to `true`. Add `ITSEncryptionExportComplianceCode` only after Apple issues the code.

Official overview: [Complying with Encryption Export Regulations](https://developer.apple.com/documentation/security/complying-with-encryption-export-regulations).  
Upload howto: [Determine and upload app encryption documentation](https://developer.apple.com/help/app-store-connect/manage-app-information/determine-and-upload-app-encryption-documentation).

---

## US EAR (parallel to Apple — not a SNAP-R license for open source)

Apple’s questionnaire does **not** replace US export-control analysis.

| Path | Typical action |
|------|----------------|
| **Publicly available** encryption source (MIT + public URL + matching object code) | Often **not subject to the EAR** under §742.15(b); usually **no SNAP-R export license**. Email notification to `crypt@bis.doc.gov` + `enc@nsa.gov` only if counsel finds **“non-standard cryptography”** (proprietary/unpublished). |
| Closed / not publicly available product crypto under License Exception ENC | May need classification / **annual self-classification** to BIS (separate from Apple). |

Keep on file for counsel:

- Public source URL for this repo  
- MIT license  
- This inventory  
- Pointers to [MESSAGE_ENCRYPTION.md](../contracts/MESSAGE_ENCRYPTION.md) and [AT_REST_ENCRYPTION.md](../contracts/AT_REST_ENCRYPTION.md)

Sanctions / denied-party rules still apply regardless of EAR encryption exclusions.

---

## One-page paste for Apple / counsel

```
Product: PP (pp-browser)
License: MIT (open source)
Encryption: Yes — not limited to Apple OS APIs
Algorithms (industry standard / NIST):
  - XChaCha20-Poly1305 (AEAD, messages + at-rest)
  - HKDF-SHA256, BLAKE2b-256, Argon2id
  - Ed25519; ML-DSA-65 (FIPS 204); ML-KEM-768 (FIPS 203)
Libraries: libsodium; OpenSSL/BoringSSL EVP; mlkem-native; mldsa-native
Primary uses: E2E P2P messaging; PIN/DEK vault; PQ account keys; TLS/Noise transport
Proprietary unpublished ciphers: No
France App Store: [Yes → French declaration | No → N/A]
US CCATS: Not expected for industry-standard path (confirm if questionnaire requires)
```

---

## Maintenance

- When shipping a new crypto primitive or dropping Ed25519 entirely, update the inventory tables and the paste block.
- After first successful Apple review, record the approved `ITSEncryptionExportComplianceCode` in the iOS/macOS bundle Info.plist (do not commit secrets; the Apple-issued code is an exemption identifier, not a signing secret).
