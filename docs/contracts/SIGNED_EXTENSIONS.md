# Signed formats: versions, extension area, algorithm ids

**Tier:** contract

**Status:** Normative for release 1, from the signed-format cutover ([release-compatibility RC020](../../projects/release-compatibility/DECISIONS.md#rc020--the-signed-formats-for-the-release-1-cutover),
[SIGNED_FORMATS § 1](../../projects/release-compatibility/SIGNED_FORMATS.md#1-the-shared-rules)). Built on the
`cutover/signed-formats` branch; on `main` from merge day. Each object's byte layout is still in
[SIGNED_FORMATS § 2](../../projects/release-compatibility/SIGNED_FORMATS.md#2-each-signed-object) until its contract
(WIRE_SCHEMAS, ACCOUNT_CARD, …) takes it in the same change that ships it.
**Related:** [COMPATIBILITY.md](COMPATIBILITY.md), [WIRE_SCHEMAS § pp Binary Wire Profile](WIRE_SCHEMAS.md#pp-binary-wire-profile-d088),
[MESSAGE_ENCRYPTION.md](MESSAGE_ENCRYPTION.md), [MESH_IDENTITY](../architecture/MESH_IDENTITY.md).

The rules every signed object shares, and the registry of what they name: one version number, one signed-bytes
header, one extension area, one table of algorithm ids. The codecs are pp-cpp-common's `common/ExtArea.h` and
pp-cpp-crypto's `crypto/AlgId.h` (the relay: `packages/shared/src/sign-payload/ext-area.ts`, `crypto/alg-id.ts`); the
app's helpers over them are in `capabilities/base/src/foundation/crypto/` ([§ Code](#code)).

---

## Versions: outside and inside

Every signed object carries **one** version number, written twice:

| Object form | Outside (read without verifying) | Inside (signed, cannot be stripped) |
|-------------|----------------------------------|-------------------------------------|
| JSON object | an integer field named per object (`card_version`, `grant_version`, …) | the `u8` after the domain |
| Binary blob | its first byte | the `u8` after the domain |

The two are the same number: a receiver rebuilds the signed bytes with the version it read outside, so a changed or
stripped outside version fails the signature. Each object has a **floor** and a **current** version; release 1's
version of each is both (it accepts only that one). After release 1 a version changes only by the
[range rule](../../projects/release-compatibility/DESIGN.md#the-range-rule).

## Signed bytes

```
domain (ASCII) ‖ 0x00 ‖ u8 version ‖ …the object's fields… ‖ ext area
```

- **Domains carry no `-vN` suffix** (`pp-browser:account-card`): the version is the byte after the NUL. Release 1
  freezes the names. Hash tags that are not signatures (`group-changes-v1`, `group-stream-v1`, the locker AAD) are not
  domains and keep theirs.
- Fields are the [pp binary wire profile](WIRE_SCHEMAS.md#pp-binary-wire-profile-d088): big endian integers, `LenUtf8`
  / `LenBytes` with a u64 length. An object keeps its own field encoding where it has one (the group objects'
  u32-length strings, the DHT record's u32 lengths); only the header, typed blobs and the area are shared.
- A typed blob ([§ Typed blobs](#typed-blobs)) is `LenBytes(typed blob)`.
- The extension area is always the **last** field. No text (`key=value`) sign bytes: they are not injective.

## The extension area

```
ext area  = u64 BE byte length of the entries ‖ entries
entry     = u16 BE type ‖ u32 BE length ‖ value (length bytes)
type      = bit 15 (0x8000): critical (must understand); bits 0–14: the id. 0x0000 and 0x8000 are invalid
```

- Entries are **strictly ascending by the whole u16 type**: no duplicates, so a set of entries has one encoding (a
  critical type sorts after every non-critical one).
- At most **4 KiB** of entries per object (headers and values; the u64 length not counted).
- Refused as malformed (step 2 of [the receiver's order](#the-receivers-order)): an invalid type, unsorted, a
  duplicate, a length past the end, 1–5 bytes left over, over 4 KiB, or (area form) bytes after the area.
- **JSON objects** carry the entries alone (no u64 length) as **`ext_b64`**, standard base64. Absent means empty, and
  `""` reads as absent; writers leave it out when empty. The signed bytes hold the decoded entries, so the JSON
  spelling never matters.
- **Binary blobs** carry the area form inside the blob where the object's layout puts it.
- Values are binary in the wire profile, never JSON. A stored or forwarded object keeps its area's bytes: unknown
  non-critical entries are ignored **and kept**.

### Extension types

Each type is defined once, here, with the objects it may appear in; a known id in an object it is not listed for
reads as unknown.

| Type | Critical | Name | Objects | Value |
|------|----------|------|---------|-------|
| — | — | none at release 1: every area is empty | — | — |

Adding a type: a row here (the next free id, the critical bit only if an older reader must refuse the object), the
objects that carry it, its value's layout, and its vectors. Ids are never reused.

## Algorithm ids

One byte names an algorithm, **counted per kind** (a field always holds one kind, so `1` is today's value wherever it
already appears). `0` is invalid in every kind. Names are the strings below wherever a name is written.

| Kind | Id | Name | Used for |
|------|----|------|----------|
| signature | 1 | `ml-dsa-65` | account, device, locker, relay transport and signing, operator keys; every signature |
| KEM | 1 | `ml-kem-768` | account and device KEM keys; `key_init` |
| AEAD | 1 | `xchacha20-poly1305` | the payload blob, `FileCipher` output, the join ticket's wrapped key, attachments |
| hash | 1 | `blake2b-256` | Account ID, locker id, relay id, copy hash, change id |
| hash | 2 | `sha2-256` | the PeerId multihash (`12 20`) |
| KDF | 1 | `hkdf-sha256` | session keys, the recovery seed's keys |

Ids are frozen: a new algorithm takes the next id of its kind. Hash and KDF ids are never written into blobs (an
object's version fixes them); they are here so docs and later fields share one vocabulary. No `*_alg` field
(`signature_alg`) is written beside a blob.

## Typed blobs

Every key, signature, KEM ciphertext and AEAD blob **starts with its algorithm id**, then the algorithm's bytes:

```
typed blob = u8 alg id ‖ body
```

| Blob | Kind | Release 1 (body → typed blob, bytes) |
|------|------|--------------------------------------|
| signing public key | signature | ML-DSA-65 1952 → **1953** |
| signature | signature | ML-DSA-65 3309 → **3310** |
| KEM public key | KEM | ML-KEM-768 1184 → **1185** |
| KEM ciphertext | KEM | ML-KEM-768 1088 → **1089** |
| AEAD sealed | AEAD | xchacha20-poly1305 nonce ‖ ciphertext ‖ tag, ≥ 40 (the payload blob and `FileCipher` bytes, unchanged) |

- JSON's base64 fields hold the typed blob (field names unchanged); signed bytes hold `LenBytes(typed blob)`.
- A reader checks, in order: empty or id 0 → **malformed**; a nonzero id it does not know for the kind → **unknown
  algorithm** (update needed, whatever follows: a later algorithm's sizes are unknown); a known id with the wrong
  body size → **malformed**.
- A signature verifies only with a key of the **same** id.
- **Identities hash the raw key, not the typed blob**: Account IDs, PeerIds and locker ids do not change.

## The receiver's order

For every signed object, in this order:

| Step | What | Failure |
|------|------|---------|
| 1. Version | the outside version | missing or below the floor: **older** (refused: made by an older app, outside the range). Above current: **update needed**, nothing parsed or verified |
| 2. Parse | that version's fields, the extension area included | **malformed** (refused) |
| 3. Algorithms | every typed blob's id | unknown: **update needed**; else malformed |
| 4. Verify | the signature over the rebuilt signed bytes, area included | **forged** (hard refusal) |
| 5. Extensions | the area's critical entries | an unknown critical entry: **update needed**, the object is not acted on |
| 6. Semantics | freshness, bounds, who may sign | the object's own |

"Update needed" is never reported as a forgery. Extensions come after the signature, unlike the version: their bytes
rebuild without being understood, and an unverified object must not raise "update needed" (anyone could make one).

In the app, **update needed** is `AppError::NewerFormat` (`AppError::IsNewerFormat`; a received message's
`ReceiveFailure::NeedsUpdate`); older, malformed and forged are plain refusals. The relay answers an older form with
`426 client_upgrade_required` and a newer one (a version, an algorithm id, a critical entry) with
`400 relay_upgrade_required` ([SIGNED_FORMATS § 2.8](../../projects/release-compatibility/SIGNED_FORMATS.md#28-relay-signed-calls-all-domains-v4)).

## Code

`capabilities/base/src/foundation/crypto/`:

| Header | What |
|--------|------|
| `TypedBlob.h` | `ReadTypedBlob` / `ReadTypedBlobB64` → `TypedRead` (`TypedBlobStatus` Ok / UnknownAlgorithm / Malformed; `ToError()`: NewerFormat / a refusal); `TypedSigningPublicKey`, `TypedSignature`, `TypedKemPublicKey`, `TypedKemCiphertext`, `WriteTypedBlob[B64]`; `SignTyped`, `VerifyTyped` |
| `SignedBytes.h` | `SignedBytesWriter(domain, version)` … `.Finish(ext)`; `SignedBlobReader` for binary blobs; `IsSignedDomain`, `SignedBytesVersion`; `ReadExtB64` / `WriteExtB64` (`kExtB64Key`) |
| `SignedReceive.h` | `ReceiveSigned(SignedReceiveSteps)` → `SignedReceipt` (`SignedVerdict`; `ToError()` / `ToRoe()`); `OutsideVersionOfJson`, `OutsideVersionOfBlob` |

The tests (`foundation/crypto/tests/signed_receive_test.cpp`) build and receive one whole signed JSON object with
these, the shape each object's reader follows.

## Vector files

Every signed object's bytes are frozen in a JSON vector file read by each side that builds them; a change to signed
bytes without a version bump fails CI. ML-DSA signing is hedged, so signatures are not compared: the signed bytes
are, and each committed signed fixture must verify.

| File | Written by | Copies |
|------|------------|--------|
| `ext_area_vectors.json` | pp-cpp-common (`tests/data/`) | pp-browser `capabilities/base/src/foundation/crypto/tests/data/`, pp-cpp-mesh, the relay |
| `alg_id_vectors.json` | pp-cpp-crypto (`tests/data/`) | pp-browser (same folder), the relay |
| per object (`relay_sign_vectors.json`, `relay_certificate_vectors.json`, …) | pp-browser, beside the object's test | the relay, pp-node |

- A copy is the pinned tag's bytes exactly (`library_vector_copies_test`); pp-browser's helpers read every case of
  it. After a library bump, `PP_WRITE_EXT_AREA_VECTORS=<path>` / `PP_WRITE_ALG_ID_VECTORS=<path>` write the pinned
  tag's file.
- pp-browser's own files use `common/tests/VectorFile.h`: the committed path from the test target's definition
  (`PP_<NAME>_VECTORS_PATH`), `PP_WRITE_<NAME>_VECTORS=<path>` to write a fresh file (two-space JSON, final newline),
  `WithoutKeys` to compare without the randomized signatures.
