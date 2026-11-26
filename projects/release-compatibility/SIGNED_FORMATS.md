# Release compatibility: signed formats for the release-1 cutover

**Status:** Design for review (Hongwei). Phase P3. Builds INVENTORY [C7](INVENTORY.md#c-a-clean-start),
[O4](INVENTORY.md#o-operating-a-live-release-rc011), [V1, V2](INVENTORY.md#v-a-version-every-receiver-checks-first),
[X1, X2](INVENTORY.md#x-worth-doing-now), T7's record version and V3's DHT half, with their frozen data (F1, F5).
**Read at:** pp-browser main `8b454806d`, pp-relay-apis main `572962a`, pp-cpp-mesh main `f9d6511`, pp-ledger main.
Paths are file names as in [INVENTORY](INVENTORY.md); `relay` and `mesh` name those repos.

Every signed format that changes before release 1 changes **once, together**: one pre-release cutover. Testers
delete the app and install release 1 ([RC017](DECISIONS.md#rc017--p0s-decisions-a-clean-local-start-the-currency-id-fixed-two-features-after-release-1)),
so [COMPATIBILITY](../../docs/contracts/COMPATIBILITY.md)'s clean cutovers are still the policy: no reader for any
older form, and **release 1 accepts only the forms below**. After release 1 each of them changes only by
[DESIGN § The range rule](DESIGN.md#the-range-rule), and the three mechanisms here are what make most later changes
additive.

Questions for Hongwei are marked **Q1 … Q16**; [§ Open questions](#open-questions) lists them with a recommendation.

## 1. The shared rules

Promoted to [docs/contracts/SIGNED_EXTENSIONS.md](../../docs/contracts/SIGNED_EXTENSIONS.md) (cutover W0), which
holds the registry (extension types, algorithm ids) and names the app's helpers; this section stays as the design's
record, and the contract wins where they differ.

### 1.1 Version outside and inside; the receiver's order

Every signed object carries one version number, written twice: **outside** (readable without verifying) and
**inside** (the byte after the domain in the signed bytes, so it cannot be stripped). The two are the same number.

| Object form | Outside | Inside |
|-------------|---------|--------|
| JSON object | an integer field named per object (`card_version`, `grant_version`, …) | `u8` after the domain |
| Binary blob | its first byte | `u8` after the domain |

The receiver, for every object:

1. **Version.** Read the outside version. Missing or below the floor (release 1's, the table in § 2): refused as
   *made by an older app* (outside the range). Above what it reads: **update needed** (`NeedsUpdate`, INVENTORY U1),
   nothing verified. Never reported as a forgery.
2. **Parse** that version's fields. Malformed (sizes, bounds, a bad extension area, § 1.2): refused as malformed.
3. **Algorithms.** Every typed blob (§ 1.3) names an id this build knows for its kind, else **update needed**.
4. **Verify** the signature over the rebuilt signed bytes, extension area included. Failure: forgery (hard refusal).
5. **Extensions.** An unknown *critical* entry: **update needed**, the object is not acted on. Unknown non-critical
   entries are ignored **and kept** (a stored or forwarded object keeps its bytes).
6. Semantics (freshness, bounds, who may sign).

Extensions are checked after the signature, unlike the version: their bytes rebuild without being understood, and
an unverified object must not raise "update needed" (anyone could make one).

### 1.2 The signed extension area

One encoding for every signed object, binary or JSON. It is always the **last field of the signed bytes**:

```
ext area  = LenBytes(entries)                    u64 BE byte length, then the entries
entry     = u16 type ‖ u32 length ‖ value        big endian; value is `length` bytes
type      = bit 15 (0x8000): critical (must understand); bits 0–14: the id. 0x0000 and 0x8000 are invalid
```

- Entries are **strictly ascending by `type`** (the whole u16): no duplicates, so one set of entries has one
  encoding. Unsorted, duplicate, invalid type, a length past the area, or bytes left over: malformed (step 2).
- Values are binary in the [pp binary wire profile](../../docs/contracts/WIRE_SCHEMAS.md#pp-binary-wire-profile-d088),
  never JSON. Each type is defined once, in one registry (a table in `docs/contracts/`, promoted in P4) that says
  which objects it may appear in; a known id in an object it is not defined for is read as unknown.
- At most **4 KiB** of entries per object (Q16). No type is defined at release 1: every area is empty.
- **JSON objects** carry the entries as `"ext_b64"`: standard base64 of `entries` (without the u64 length). Absent
  means empty; writers omit it when empty. The signed bytes hold the decoded entries, so the JSON spelling never
  matters (**Q1**: this TLV form, against a JSON map).
- **Binary blobs** carry the area inside the blob at the place the table of each object shows.
- Same semantics as media frames' extension block ([WIRE_SCHEMAS § Media frames](../../docs/contracts/WIRE_SCHEMAS.md#media-frames):
  1-byte type and length, `0x80` must understand), wider fields. The same entry encoding serves INVENTORY T4
  (ChatPayload tails) and X5's binary formats; X3 (MSH v2) designs its own inside the transcript.
- One codec: `common/ExtArea.{h,cpp}` in pp-cpp-common (used by pp-browser and pp-cpp-mesh), and
  `packages/shared/src/sign-payload/ext-area.ts` in relay, both reading one frozen vector file (§ 6).

### 1.3 Algorithm names: one registry, typed blobs

| Kind | Id | Name | Where it is used |
|------|----|------|------------------|
| signature | 1 | `ml-dsa-65` | account, device, locker, relay transport and signing, operator keys; every signature |
| KEM | 1 | `ml-kem-768` | account and device KEM keys; `key_init` |
| AEAD | 1 | `xchacha20-poly1305` | the payload blob, `FileCipher` (locker `sealed`, at-rest files), the join ticket's wrapped key, attachments (X4) |
| hash | 1 | `blake2b-256` | Account ID, locker id, relay id, copy hash, change id |
| hash | 2 | `sha2-256` | the PeerId multihash (`12 20`) |
| KDF | 1 | `hkdf-sha256` | session keys, the recovery seed's keys |

Ids count per kind: a field always holds one kind, so `1` is today's value everywhere it already appears
(`signature_alg` u8 `1` at registration, `RelaySignBytes.cpp:60-65`; the payload blob's first byte, `EncryptedPayload.cpp:19-27`).
Names are the strings above wherever a name is written. `0` is invalid; the registry lives beside the extension
types.

**Typed blob:** every key, signature, KEM ciphertext and AEAD blob **starts with its algorithm id** (one byte),
then the algorithm's bytes. In JSON the base64 field holds the typed blob (field names unchanged); in signed bytes a
typed blob is `LenBytes(typed blob)` (sizes now vary by algorithm). A signature verifies only with a key of the same
id. (**Q2**: typed blobs, against an `*_alg` field beside each blob.)

- **What changes size:** keys and signatures gain one byte (ML-DSA-65 key 1953, signature 3310; ML-KEM-768 key
  1185, ciphertext 1089). The payload blob and `FileCipher` output do **not** change: their first byte is already
  `1`, now read as the AEAD id (`CryptoConstants.h:9`, `FileCipher.cpp:32`).
- **Identities hash the raw key, not the typed blob:** Account IDs, PeerIds ([MESH_IDENTITY](../../docs/architecture/MESH_IDENTITY.md),
  frozen) and locker ids do not change, so an account restored from its phrase keeps its Account ID. The forms name
  their algorithm: a PeerId through key type 4 in its hashed encoding, `account:` and `relay:` (§ 3) are ML-DSA-65
  and BLAKE2b-256 by definition; a later algorithm gets a new form.
- Hash and KDF ids are not written into blobs: an object's version fixes them. They are in the registry so docs and
  later fields use one vocabulary.
- `signature_alg` fields go: registration's body and sign-bytes byte, register/start's answer, the directory row,
  `relay-identity`, the DHT record (each named in § 2).
- Codec: `crypto/AlgId.h` and a typed-blob helper in pp-cpp-crypto; `crypto/alg-id.ts` in relay.

### 1.4 Signed bytes

Every object's signed bytes start the same way, as most do today ([SERVICE_ENDPOINTS § The audience](../../docs/contracts/SERVICE_ENDPOINTS.md#the-audience-every-signed-command-names-its-relay)):

```
domain (ASCII) ‖ 0x00 ‖ u8 version ‖ …the object's fields… ‖ ext area
```

- **Domains lose their `-vN` suffix** at this cutover (`pp-browser:account-card`, not `-v4`): every signed byte string
  changes anyway, release 1 freezes the names for good, and today two numbers disagree (card domain `-v4`, envelope
  `-sign-v5` with an `envelope_version` 3 byte after it). (**Q3**.) Hash tags that are not signatures
  (`group-changes-v1`, `group-stream-v1`, the locker AAD) are not touched.
- Field encodings stay each object's own where they differ (the group objects keep their u32-length strings,
  `GroupRosterStatement.cpp:22-35`; the DHT record its u32 lengths): only the header, typed blobs and the area are
  shared.
- Text sign bytes (`key=value\n` lines: the join ticket, the announce tip) become binary. The lines are not
  injective (a value holding `\n` moves fields, `BroadcastJoinTicket.cpp:17-19`, `PeerAnnounceCodec.cpp:122-150`) and
  carry no domain.

## 2. Each signed object

Release 1's version of each is its **floor and its only accepted version**.

| Object | Outside: today → release 1 | Inside: today → release 1 | Signed by | Today's code |
|--------|----------------------------|---------------------------|-----------|--------------|
| Envelope | `envelope_version` 3 → **4** | sign 5 + envelope 3 → **4** | sending device | `EnvelopeSigner.cpp:21-27, 121-180`, `MessagingJson.cpp:374-389` |
| Account card | `card_version` 4 → **5** | 4 → **5** | account | `AccountCard.cpp:21-23, 67-87, 210, 281-317` |
| KEM binding | none → blob byte **2** | 1 → **2** | account | `KemKeyBinding.cpp:11, 26-28` |
| Relay grant | none → `grant_version` **2** | 1 → **2** | recipient account | `RelayGrantSigner.cpp:13-27`, `MessagingJson.cpp:552-575` |
| Roster statement, dissolved | none → `statement_version` **4** | tag `-v3` only → **4** | owner account | `GroupRosterStatement.cpp:18-19, 249-269, 454-530` |
| Owner transfer | none → `transfer_version` **2** | tag `-v1` only → **2** | previous owner account | `GroupRosterStatement.cpp:20, 272-280, 431-452` |
| Roster change | none → `change_version` **2** | tag `-v1` only → **2** | member device | `GroupChange.cpp:15, 266-281, 335-380` |
| Locker record | none (`version` is a revision) → `record_version` **2** | 1 → **2** | locker key | `AccountLocker.cpp:24-25, 74-81`, `HttpLockerClient.cpp:31-44` |
| Locker state | none → `state_version` **1** (sealed, not signed) | — | sealed by the locker key | `AccountLocker.cpp:131-184` |
| `PeerRequestAuth` | none → `auth_version` **2** | none → **2** | requester account | `PeerRequestAuth.cpp:21-38` |
| Relay signed calls | `sign_version` 3 / 3 / 2 → **4** for all | same → **4** | account (or node) | `RelaySignBytes.cpp:15-24`, relay `parse.ts:54-76`, `builders.ts:30-59, 348-354` |
| Broadcast join ticket | `schema_version` 1 → **2**, compared first | `v=1` line → **2** | publisher device | `BroadcastJoinTicket.cpp:40-56, 88-89, 169-206` |
| Peer-announce tip | `schema_version` 1 → **2** | `v=1` line → **2** | publisher device | `PeerAnnounceCodec.cpp:122-150, 203-205` |
| Relay certificate | new: blob byte **1** | **1** | operator set | none ([§ 3](#3-the-relay-operator-set-and-certificate)) |
| Operator-set change | new: blob byte **1** | **1** | previous operator set | none |
| DHT record | `version` 1, never read → **2**, read first | `sign_version=1` line → **2** | node device | mesh `DhtRecordCodec.cpp:32-50, 58, 76, 96, 141` |

All relay command domains move to one sign version, **4** (**Q4**): today's table of three
(`currentSignVersion`, relay `parse.ts:54-58`) becomes one constant.

### 2.1 Envelope (v4)

```
"pp-browser:relay-envelope" 00 ‖ u8 4
‖ u8 route_kind ‖ u8 channel ‖ i64 timestamp
‖ LenUtf8 message_id ‖ LenUtf8 sender_contact_id ‖ LenUtf8 sender_device
‖ u64 copies, by recipient_device: LenUtf8 recipient_device ‖ u32 session_epoch ‖ u64 sender_seq
                                   ‖ LenUtf8 key_init_b64 ("" when none) ‖ copy hash (32)
‖ ext area
```

- JSON: `envelope_version: 4`, `ext_b64` at top level. The separate `sign_version` byte goes: one version.
- Typed: `signature` (device key's id); `key_init_b64` (KEM id 1, then the ciphertext); `payload_b64` (AEAD id 1:
  unchanged bytes). The copy hash stays BLAKE2b-256(`0x02` ‖ LenBytes(payload)).
- No extension area per copy: what is per device belongs in the copy's ciphertext (ChatPayload, T4) (**Q14**).
- Still unsigned: `sender_card` (signed by its account), `sender_card_at`, relay metadata, unknown keys.
- Receiver: a higher `envelope_version` is T2's placeholder with its seqs closed; unknown critical extension the same.

### 2.2 Account card (v5) and key card

```
"pp-browser:account-card" 00 ‖ u8 5
‖ LenBytes account signing key (typed) ‖ LenBytes account KEM key (typed)
‖ LenUtf8 nickname
‖ u64 devices, by peer_id bytes: LenUtf8 peer_id ‖ LenBytes signing key (typed) ‖ LenBytes KEM key (typed)
‖ u64 relays, in list order:     LenUtf8 origin ‖ relay_id (32 raw)
‖ i64 initiation_floor ‖ i64 issued_at
‖ ext area
```

- JSON: `card_version: 5`; `relays: [{ "origin": "https://…", "relay_id": "relay:…" }]` in place of `peer_id`
  (C7, § 3.3); typed keys; `card_signature_b64` typed; `ext_b64`. A device's `peer_id` stays (it is the device's).
- Bounds as today ([ACCOUNT_CARD § Account card](../../docs/contracts/ACCOUNT_CARD.md#account-card)) with `relay_id`
  for `peer_id`: each `relay_id` of the `relay:` form, none twice. A device's keys may be of different ids than
  another's: an algorithm migration happens per device.
- The **key card** (`account_id`, signing key, KEM key, binding) keeps its four fields, typed, with
  `kem_binding_b64` (§ 2.3) for `kem_binding_signature_b64`. The directory row returns the same.
- Cards are stored and forwarded as received, unknown keys and extensions included (relay T8; app X8).
- Receiver: `card_version > 5` or an unknown critical extension: the peer *needs a newer app* (U2), the card-ask
  gate stops asking for a version it cannot read.

### 2.3 KEM binding (v2)

```
signed: "pp-browser:kem-binding" 00 ‖ u8 2 ‖ LenBytes account signing key (typed) ‖ LenBytes KEM key (typed) ‖ ext area
blob:   u8 2 ‖ ext area ‖ LenBytes signature (typed)          → `kem_binding_b64` in key cards, directory rows, register/finish
```

The binding is a signature that rides other objects, so its version is the blob's first byte rather than a JSON
field in each carrier.

### 2.4 Relay grant (v2)

```
"pp-browser:relay-grant" 00 ‖ u8 2 ‖ LenUtf8 recipient_account_id ‖ LenUtf8 sender_account_id
‖ i64 issued_at ‖ i64 expires_at ‖ ext area
```

JSON (on a send's `grant`, the lane's `relay_grant`, the locker's `grants`): `grant_version: 2`, `ext_b64`, typed
`signature`. A grant the app cannot read is treated as none, and its grantor as *needs a newer app*: it is not asked
for again in a loop (today a newer grant reads as a forgery and resends hourly, INVENTORY § 2.1).

### 2.5 Group roster statement (v4), dissolved, owner transfer (v2), roster change (v2)

```
statement: "pp-browser:group-roster-statement" 00 ‖ u8 4 ‖ (today's fields from group_id on, u32-length strings) ‖ ext area
dissolved: "pp-browser:group-dissolved"        00 ‖ u8 4 ‖ (the same)                                         ‖ ext area
transfer:  "pp-browser:group-owner-transfer"   00 ‖ u8 2 ‖ group_id ‖ from ‖ to ‖ u64 roster_epoch            ‖ ext area
change:    "pp-browser:group-roster-change"    00 ‖ u8 2 ‖ (today's fields from group_id on)                    ‖ ext area
```

- JSON: `statement_version: 4` (a dissolved one too; `dissolved: true` picks the domain), each `owner_chain` entry
  `transfer_version: 2`, each change `change_version: 2`; `ext_b64` on each; `owner_public_key`, `from_public_key`
  and every signature typed.
- Folded in (T6): the member role, the preset and the change kind enter the signed bytes as the **strings received**,
  not rebuilt from the enum (`GroupRosterStatement.cpp:50-58`, `GroupChange.cpp:274`), so an unknown value still
  verifies and is then read as unknown.
- The change id stays BLAKE2b-256 of the change's signed bytes (`GroupChange.cpp:279-281`): every id changes at the
  cutover, and `changes_hash` with it. Nothing older is read.
- Group invites carry no signature of their own (links are signed inside the statement); the join link text is U3.

### 2.6 Account locker record (v2) and state

```
"pp-browser:account-locker" 00 ‖ u8 2 ‖ LenBytes locker_id ‖ u64 version ‖ LenBytes sealed ‖ ext area
```

- JSON record (`POST /v1/lockers/put`, `GET`, the saved copy's `locker`): `record_version: 2`, `locker_id`,
  `version` (the revision, as today), `signing_public_key_b64` (typed), `sealed_b64` (AEAD id 1: unchanged
  `FileCipher` bytes), `ext_b64`, `signature_b64` (typed). The seal's AAD is unchanged.
- **State JSON** (inside the seal; T7): `state_version: 1` checked first after opening. Higher: the device says
  *update needed* and does not write. Writers keep unknown keys and unknown row fields (`AccountLocker.cpp:131-184`,
  `AccountStateCodec.cpp:14-35`). `relays.list` entries become `{origin, relay_id}`; `grants` hold grant v2.
- The saved-copy file keeps `format: "pp-browser-account-locker-file-v1"`: its shape does not change.
- Receiver: a higher `record_version` is skipped by `PickNewest` as *made by a newer app*, and the device stops
  publishing with that notice (today: "does not verify", `AccountLockerWorkflow.cpp:315-318, 467-472`).

### 2.7 `PeerRequestAuth` (v2): history, blob, account-card requests

```
"pp-browser:peer-request" 00 ‖ u8 2 ‖ LenUtf8 kind ("history" | "blob" | "account-card")
‖ LenUtf8 requester_account_id ‖ LenUtf8 requester_peer_id ‖ i64 timestamp_ms ‖ (the kind's fields, as today) ‖ ext area
```

JSON `auth`: `auth_version: 2`, `ext_b64`, typed `signature`; `requester_keys` is a key card (§ 2.2). A server that
cannot read the version or a critical extension answers a coded refusal (`{"ok":false,"code":"update_needed"}`,
with T12), never a silent close.

### 2.8 Relay signed calls (all domains, v4)

```
domain ‖ 00 ‖ u8 4 ‖ LenUtf8 audience ‖ …the domain's fields (SERVICE_ENDPOINTS table)… ‖ ext area
relay-api: "pp-browser:relay-api" 00 ‖ u8 4 ‖ LenUtf8 audience ‖ u8 op ‖ i64 timestamp ‖ …the op's fields… ‖ ext area
register:  "pp-browser:relay-register" 00 ‖ u8 4 ‖ LenUtf8 audience ‖ LenUtf8 challenge
           ‖ LenBytes account signing key (typed) ‖ LenBytes account KEM key (typed) ‖ i64 timestamp ‖ ext area
```

- Domains: the 20 of [SERVICE_ENDPOINTS § The audience](../../docs/contracts/SERVICE_ENDPOINTS.md#the-audience-every-signed-command-names-its-relay)
  without their `-v1` (`pp-browser:relay-revoke`, `…:relay-locker-put`, …). Fields otherwise unchanged; register
  drops its `signature_alg` byte.
- Body: `sign_version: 4` (read first by the relay, as today), `audience` (the relay's transport PeerId, from its
  certificate, § 3.4), `ext_b64` (signed), typed `signature`; `public_key`, `kem_public_key_b64` and
  `sender_public_key_b64` typed; `kem_binding_b64` on register/finish.
- Relay answers: older or none → `426 client_upgrade_required`; newer, an unknown algorithm id or an unknown
  critical extension → `400 relay_upgrade_required` (U10, [RC018](DECISIONS.md#rc018--the-release-1-answers-for-kenneths-and-leos-rows)
  item 9); unknown non-critical entries and unknown body keys ignored.
- The legacy window (`isLegacyCall`, relay `parse.ts:78-91`) is removed (C2).

### 2.9 Broadcast join ticket (v2) and peer-announce tip (v2)

```
ticket: "pp-browser:broadcast-join-ticket" 00 ‖ u8 2 ‖ LenUtf8 publisher_peer_id ‖ LenUtf8 program_id
        ‖ LenUtf8 join_handle ‖ LenUtf8 viewer_peer_id ‖ u64 media_epoch ‖ LenUtf8 media_key_id
        ‖ LenBytes wrapped_key (typed AEAD blob, empty if none) ‖ LenBytes key_material (empty if none)
        ‖ LenUtf8 hop_peer_id ("" if none) ‖ i64 expires_at_ms ‖ ext area
tip:    "pp-browser:peer-announce-tip" 00 ‖ u8 2 ‖ LenUtf8 peer_id ‖ LenUtf8 topic_id ‖ LenUtf8 program_id
        ‖ LenUtf8 state ‖ u64 seq ‖ u64 epoch ‖ i64 created_at_ms ‖ LenUtf8 join_handle ‖ LenUtf8 hop_peer_id
        ‖ u64 n ‖ n × LenUtf8 l1_hop_peer_id ‖ u64 n ‖ n × u8 video_level ‖ LenUtf8 kind ‖ LenUtf8 viewer_peer_id
        ‖ LenUtf8 viewer_msg_id ‖ LenUtf8 body ‖ LenUtf8 content_id_hex ‖ ext area
```

- JSON: `schema_version: 2`, `ext_b64`, typed `signature_b64`. The ticket's `schema_version` is compared before
  verifying (V2; today it is read with a default and never compared, `BroadcastJoinTicket.cpp:88-89`).
- Every tip field is always signed (empty string or empty list when absent): "signed when present" broke older
  verifiers on any new field (INVENTORY § 2.4). New fields go into the extension area.

### 2.10 DHT record (v2): in scope

In scope (**Q15**): pp-node 10.0.0 and the apps ship together at release 1 (the last lockstep), and V3 and X1–X2 already
name it. The encoding is pp-cpp-mesh's and keeps its u32 lengths:

```
"pp-mesh:dht-record" 00 ‖ u8 2 ‖ (today's fields from `type=peer_routing` on, DhtRecordCodec.cpp:36-49) ‖ ext area
```

JSON `version: 2`, read before verifying (V3); `ext_b64`; typed `signature_b64` and `public_key_b64`; the unsigned
`signature_alg` field goes (`DhtRecordCodec.cpp:76, 96, 141`). A newer record is skipped as *update needed* and not
counted as misbehaviour.

## 3. The relay operator set and certificate

[RC018](DECISIONS.md#rc018--the-release-1-answers-for-kenneths-and-leos-rows) item 7, modelled on pp-ledger's beacon
authority ([BEACON_AUTHORITY](https://github.com/people-post/pp-ledger/blob/main/docs/contracts/BEACON_AUTHORITY.md);
[B002, B006, B014](https://github.com/people-post/pp-ledger/blob/main/projects/beacon-authority/DECISIONS.md)): an
offline set certifies a hot key, the hot key cannot certify its successor, and nodes keep the highest certificate
from any channel.

| pp-ledger | Relay | Notes |
|-----------|-------|-------|
| admin set (genesis keys, `minSignatures`) | **operator set** (keys, threshold), offline | what clients pin, as its hash: the **relay id** |
| `T_CONFIG` (admin set replaces itself) | **operator-set change**, signed by the set in force | keeps the relay id |
| beacon signing key, `T_BEACON_CERT` generation | relay **signing key**, certificate **sequence** | hot on the deployment |
| beacon Amp key: transport only, unsigned dial hint | relay **transport key** (its PeerId): **certified** | signed calls name it as their audience |
| certificate out of band: highest kept, never below | the same, from `relay-identity`, `client-compat`, a build | |

### 3.1 Formats

```
OperatorSet  = u8 threshold ‖ u8 n ‖ n × LenBytes(typed signature key)      keys sorted by bytes, distinct
               1 ≤ threshold ≤ n ≤ 8

relay id     = BLAKE2b-256( "pp-browser:relay-id" 00 ‖ u8 1 ‖ OperatorSet_founding )
written      = "relay:" ‖ base64url-unpadded(relay id)                       49 characters (Q5)
```

**Relay certificate** (`certificate_b64` = base64 of the blob):

```
blob    = u8 version (1) ‖ LenBytes body ‖ u8 m ‖ m × ( u8 signer_index ‖ LenBytes signature (typed) )
signed  = "pp-browser:relay-certificate" 00 ‖ u8 1 ‖ body
body    = LenBytes OperatorSet_founding                      → the relay id
        ‖ u64 k ‖ k × LenBytes(operator-set change blob)      0 in release 1's operations (§ 3.6)
        ‖ u64 sequence                                         ≥ 1, rising
        ‖ i64 issued_at                                        ms; informational
        ‖ i64 expires_at                                       ms; 0 = none (release 1 writes 0, Q7)
        ‖ u64 o ‖ o × LenUtf8 origin                           1 ≤ o ≤ 4: where this relay answers (Q6)
        ‖ LenBytes transport key (typed)                       its PeerId (pp::PeerIdFromMlDsaPublicKey) is the audience
        ‖ LenBytes signing key (typed)
        ‖ ext area
```

`signer_index` points into the **operator set in force**: the founding set after the certificate's changes.
**Operator-set change:**

```
blob    = u8 version (1) ‖ LenBytes body ‖ u8 m ‖ m × ( u8 signer_index ‖ LenBytes signature (typed) )
signed  = "pp-browser:relay-operator-set-change" 00 ‖ u8 1 ‖ relay id (32) ‖ body
body    = u64 set_generation (2, 3, …) ‖ LenBytes OperatorSet_new ‖ ext area     signed by the set it replaces
```

Size: a 2-of-3 certificate is about 16.6 KB (5 keys, 2 signatures), 22 KB in base64.

### 3.2 Who signs, and the rules

- **The operator set signs only certificates and set changes.** A certificate is valid when: its version is known;
  its founding set hashes to the pinned relay id; each set change, in order, carries `set_generation` one above the
  last and `threshold` distinct valid signatures of the set it replaces; the certificate carries `threshold`
  distinct valid signatures of the set in force (no index twice); `1 ≤ o ≤ 4` origins, each within the card's origin
  bounds; transport and signing keys distinct, neither an operator key; `expires_at` is 0 or in the future; no
  unknown critical extension.
- **Sequence:** clients keep, per relay id, the valid certificate with the highest `sequence` they have seen and
  never accept a lower one. A second certificate at the held sequence with other bytes is refused and logged as an
  error (the first stays, as pp-ledger's out-of-band rule). Operators never sign two certificates at one sequence;
  the tool keeps a log.
- **The relay's own keys certify nothing:** a stolen transport or signing key cannot move clients to the thief's.
  Release 1 bounds a stolen key only by spreading the new certificate (RC018 part 2's default: no expiry); the
  `expires_at` reader ships anyway, so expiry can start later without a format change.
- **The signing key** is hot on the deployment (a second seed beside the identity seed). It signs nothing in
  release 1; it is certified now so that items the relay issues later need no certificate change (**Q13**).

### 3.3 What carries the relay id instead of `peer_id`

| Carrier | Today | Release 1 | Code |
|---------|-------|-----------|------|
| Account card `relays[]` | `{origin, peer_id}` | `{origin, relay_id}` (§ 2.2) | `AccountCard.cpp:67-87`, relay `account-card.ts:107-146` |
| Locator `relay` | `{origin, peer_id}` | `{origin, relay_id}`; prefix stays `pp-browser-contact-v1:` (nothing older exists) | `ContactLocator.cpp:21, 44, 99` |
| Relay link | `https://host/api/relay#Qm…` | `https://host/api/relay#relay:…` | `RelayOrigins.cpp:310-333`, relay `relay-identity.ts:33` |
| My relays, pins | `our_relays.json` `peer_id`, `relay_pins.json` per origin | `relay_id`; pins keyed by relay id, holding the highest certificate | `RelayPins.cpp:273-330` |
| Locker `relays` | `{origin, peer_id}` | `{origin, relay_id}` (§ 2.6) | `AccountLocker.cpp:131-184` |
| `contacts.json` `remote.locator_relay` | `{origin, peer_id}` | `{origin, relay_id}` | |
| Builds | `PP_RELAY_PEER_ID_PROD` / `_QA` | `PP_RELAY_ID_PROD` / `_QA` / `_DEV`, and the environment's certificate (§ 3.5) | `DeploymentProfile.cpp:16-22, 56-63`, `check_relay_peer_ids.sh` → `check_relay_ids.sh` (`^relay:[A-Za-z0-9_-]{43}$`) |
| pp-node | `registration.peer_id`, `PP_NODE_REGISTRATION_PEER_ID` | `registration.relay_id`, `PP_NODE_REGISTRATION_RELAY_ID` | `node_env_overlay_test.cpp:156-188` |

The local stores change shape with no migration step: their release-1 version is this shape and is their floor
(C4, RC017 item 1). This amends [MR008](../multi-relay/DECISIONS.md#mr008--a-relay-address-carries-its-peerid-no-first-use-trust):
a relay address is its origin and its **relay id**; a new PeerId (a new host, a leaked key) is a new certificate,
not a new relay.

### 3.4 How the audience PeerId is learned and verified

1. The client holds a relay id from a trusted source: the build, a relay link, our own card on a linked device, the
   locker on a restore; for a contact's relay, the contact's card or locator (the inviter's word, as today).
2. It gets the certificate from `GET {origin}/v1/relay-identity` (and, for the org's relay, `client-compat`), checks
   it against the relay id (§ 3.2) and that **the origin it asked is one the certificate lists**, and keeps it if
   higher than the one held.
3. The **audience** of every signed call to that relay is the PeerId of the held certificate's transport key. A
   PeerId is never taken from an unsigned answer.
4. `401 wrong_audience`: fetch the certificate again (at most once per relay per 5 min, as today), adopt a higher
   one, retry once. A relay serving a certificate of another relay id, or none valid, *does not match its address*:
   not used, logged, left off our card ([SERVICE_ENDPOINTS § Pins](../../docs/contracts/SERVICE_ENDPOINTS.md#pins)
   as today, with relay id for PeerId).
5. A contact's relay is now checked too: its certificate proves the PeerId, where today the card's `peer_id` is only
   the inviter's word. It is cached under its relay id (it proves itself) and never makes the relay ours.

Binding origins closes MR008 item 6: a hostile relay that proxies another relay's answers serves a certificate that
does not list its origin, so it is not used; and it cannot make one that does without the other relay's operators.

`GET /v1/relay-identity` answers `{ "schema_version": 2, "relay_id": "relay:…", "certificate_b64": "…" }`
(`peer_id`, `public_key_b64` and `signature_alg` go). `client-compat` gains `relay_certificate_b64` (additive at its
`schema_version` 1, RC018 item 3). Readers accept a higher `schema_version` for the fields they know (T15).

### 3.5 Bootstrap

- **The org relay:** each build carries, per environment, the relay id (`PP_RELAY_ID_<ENV>`) and the newest
  certificate at build time (`config/relay-certs/<env>.cert`) as a floor: a fresh install knows the audience before
  its first fetch, works offline at start (pp-node), and never goes below the certificate it shipped with (**Q10**).
  A release workflow refuses an empty or malformed relay id for prod; a dogfood build without one pins the first
  valid certificate's relay id, warned (as `OrgRelayFirstUseAllowed` today).
- **Environments** ([RC009](DECISIONS.md#rc009--four-environments-todays-qa-becomes-staging-dev-is-new)): prod and
  staging (today's qa) each have their own operator set; every environment runs a 1 of 1 set today, and 2 of 3, offline, one key in escrow, is the target for prod and staging (RC018 part 2,
  Leo), reached by an operator-set change (§ 3.2 / § 3.6); the local stack stays 1 of 1 (**Q9**). The local stack generates its set and certificate at start and
  names the link in `/_local/status`.
- **User-run relays:** the operator runs relay's `scripts/relay-operator.mjs` (§ 4): generates the operator keys
  (1 of 1 is allowed), the transport and signing seeds, signs certificate 1 for its origins, and prints the relay
  link `https://host/api/relay#relay:…`. People add the relay by that link; a developer build may add a bare origin
  and pin the relay id of the first valid certificate (MR008 item 4, unchanged).

### 3.6 Rotation and recovery

| Event | Who | What clients see |
|-------|-----|------------------|
| New transport or signing key (a host move, a leak) | operators sign certificate *n*+1 for the new keys; Leo deploys the new seeds and certificate | the higher certificate at the next `relay-identity` or `client-compat` check, or after a `wrong_audience` |
| One operator key lost or leaked | the set in force signs an operator-set change; then a certificate carrying it | the same; the relay id does not change |
| Every operator key lost | none: a new set is a new relay id, a new relay link, a new build | cards re-issued with the new id |

Release 1 ships the set-change **reader**; using it waits for its runbook (**Q8**). During a planned key rotation the
functions do not switch at one instant, so the relay accepts the previous transport PeerId as audience for a short
window after deploying a new certificate, never after a compromise (**Q11**, Leo).

## 4. What changes on the relay (pp-relay-apis)

| Item | Change | Files |
|------|--------|-------|
| Shared codecs | the extension area, typed blobs and the algorithm registry (§ 1.2–1.3) | new `sign-payload/ext-area.ts`, `crypto/alg-id.ts` |
| Account card v5 | parse `{origin, relay_id}`, typed keys, the v5 signed bytes with the area; **store and serve the card verbatim** (T8): the JSON received, ≤ 64 KiB, unknown keys and entries kept, `card_issued_at` from the verified card; v1–4 → `426`, v6+, an unknown id or critical entry → `relay_upgrade_required`; the floor read from it as today | `account-card.ts:17-18, 155-180, 196-221, 251-298`, `repos/accounts.ddb.ts:189-227`, `registration.ts:290` |
| KEM binding v2 | `kem_binding_b64` on register/finish, verified and stored verbatim, returned on directory rows | `builders.ts:70-75`, `registration.ts:103-106` |
| Grant v2 | the v2 bytes; `grant_version` read first on a send | `grant.ts:5, 21-24`, messenger `send.ts` |
| Locker record v2 | `record_version` first, typed key and signature, the area; stored verbatim | `crypto/locker.ts:10, 33-39`, registry `lockers.ts:34-41` |
| Sign domains | every domain at version 4 without its `-v1`; `ext_b64` signed in every body; typed `signature` and keys; `signature_alg` gone from register/start and finish and the directory row; `relay_upgrade_required`; the legacy window and `LEGACY` builders removed (C2) | `sign-payload/builders.ts:30-59, 348-354`, `parse.ts:54-91`, `auth.ts` (`SignDomain`) |
| Relay identity | the registry loads the certificate (a secret of the deployment: 22 KB is too large for a function's 4 KB environment) and checks it at load: valid under `RELAY_ID`, its transport key the identity seed's, its signing key the signing seed's, else `503 relay_identity_unavailable`; serves `relay-identity` v2 and `client-compat`'s `relay_certificate_b64`. Other functions keep `RELAY_IDENTITY_PEER_ID` for the audience check, set by the deploy from the certificate | registry `relay-identity.ts:21-106`, `client-compat.ts:65-71`, shared `relay-identity.ts:106-117` |
| Tools | `scripts/relay-operator.mjs`: `keygen` (one holder's key), `make-cert` (unsigned body: set, sequence, origins, the deployment's public keys), `sign` (one holder), `combine`, `verify --relay-id`, `link`; `relay-identity-keygen.mjs` also makes the signing seed | `scripts/` |
| Vectors | `relay-sign-vectors.json` v2 copied byte for byte; new `ext-area-vectors.json` and `relay-certificate-vectors.json` (§ 6) | `sign-payload/*.test.ts` |
| Cutover data | delete what release 1 cannot read: stored v4 cards, record-1 lockers, mailboxes of pre-release envelopes (**Q12**) | one-time job with relay 1.1 |

## 5. What changes in the app and pp-node (pp-browser)

One PR series on main, each object with its vectors: the shared helpers (pp-cpp-common `ExtArea`, pp-cpp-crypto
`AlgId` and seeded keygen goldens, F3), then § 2's objects (files in its table), then § 3: `RelayCertificate` in
`lib/net` (parse, verify, keep the highest), `RelayPins` keyed by relay id and holding certificates,
`RelayIdentity` v2, `ClientCompat`'s certificate, `RelayAudience` from the certificate, `ParseRelayLink`,
`DeploymentProfile` and the build scripts, pp-node's `registration.relay_id`. Contracts updated in the same series:
ACCOUNT_CARD, WIRE_SCHEMAS (envelope, announce tip, versioning matrix), MESSAGE_ENCRYPTION, ACCOUNT_LOCKER,
SERVICE_ENDPOINTS (relay identity, pins, the audience table), MESH_IDENTITY (the relay link), and a new
[`docs/contracts/SIGNED_EXTENSIONS.md`](../../docs/contracts/SIGNED_EXTENSIONS.md) holding § 1's registry (P4 folds it into the version table).

## 6. Frozen test vectors

Every vector is read by both sides that build the bytes (C++ and TS for the relay's objects); a change to signed bytes
without a version bump fails CI ([DESIGN § Tests](DESIGN.md#tests), INVENTORY F1, F5). ML-DSA signing is hedged, so
signatures are not regenerated identically: the **signed bytes** are frozen (hex or SHA-256), and each signed
fixture is frozen as a whole and must verify.

| Set | Contents | Where (written by → read by) |
|-----|----------|------------------------------|
| Extension area | empty; one non-critical; one critical; two sorted; refused: unsorted, duplicate, type `0x0000` / `0x8000`, length past the end, bytes left over, over 4 KiB | pp-cpp-common → pp-browser, mesh, relay |
| Typed blobs | each registry id with its size; id 0; an unknown id per kind | pp-cpp-crypto → pp-browser, relay |
| Per object (§ 2, every row) | the signed bytes of a fixed input with an empty area, and with one unknown non-critical entry (verifies, entry kept); a signed fixture; refusals: version + 1 (update needed before verify), version − 1 (older), unknown critical entry (verifies, then update needed), unknown algorithm id | pp-browser (`envelope_signer_test`, `account_card_test`, group, locker, `PeerRequestAuth`, broadcast tests), mesh (DHT) |
| `relay_sign_vectors.json` v2 (F5) | every domain and op at version 4 for relays `a` and `b`; a card v5 and the refused cards (v4 → `426`, v6, an unknown critical entry and an unknown id → `relay_upgrade_required`, today's bounds); KEM binding v2; grant v2; locker record v2; version 3 → `426`, version 5 → `relay_upgrade_required`; an unknown body key and an unknown non-critical entry (accepted); response fixtures with unknown keys (register/finish, poll, by-account, `client-compat`, `relay-identity` v2) | pp-browser → relay (byte for byte) |
| `relay_certificate_vectors.json` | operator keys from seeds `0x01…`, `0x02…`, `0x03…` (2 of 3 in the vectors); the relay id; certificates 1 and 2 (a new transport key); one with a set change; refused: one signature, an index twice, a key outside the set, another relay id, a lower sequence, the same sequence with other bytes, an origin not listed, `expires_at` past, an unknown critical entry, version 2 | pp-browser → relay, pp-node |
| Seeded keygen (F3) | the account, device, locker, operator and relay keys from fixed seeds: fixed key hashes | pp-cpp-crypto |

## 7. The cutover plan

Release 1 accepts only the forms in § 2 and § 3, on every side. The order:

1. **Agree this design**; freeze § 1's registry and § 2's byte layouts. pp-browser writes the vector files first.
2. **Libraries:** pp-cpp-common (`ExtArea`), pp-cpp-crypto (`AlgId`, seeded-keygen goldens), pp-cpp-mesh (DHT record
   v2), each tagged and pinned.
3. **Relay readers first:** pp-relay-apis main reads only the new forms (§ 4) and serves the certificate; merged
   **the same day as step 4**, so main's builds are broken against qa for hours, not days (RC019: main deploys qa).
   Staging's operator ceremony (or its 1-of-1 set) happens before the merge.
4. **App and pp-node:** pp-browser main writes and reads only the new forms; the builds carry staging's and dev's
   relay ids and certificates. The lab runs against the qa relay.
5. **Prod ceremony** (Leo): the prod operator set signs certificate 1 for the prod relay's **current** transport key
   (no rotation needed) and a new signing seed; the relay id and certificate go into the release build's
   configuration.
6. **Release day**, in this order: app 1.1.0 (RC024) and pp-node 10.0.0 approved and held; relay 1.1 deployed to prod from
   its tag with the certificate, `min_client_version` 1.1.0 and `min_node_version` 10.0.0, and the cutover data job
   (Q12); then the apps released. Between the deploy and the release, older apps get `426` and say *update*.
   Testers delete the app and install release 1 (RC017); restoring from the phrase finds no pre-release locker
   (Q12) and starts with an empty locker under the same Account ID.

Rollback ([RC003](DECISIONS.md#rc003--the-relay-can-always-roll-back-one-release)) starts at relay 1.1: it is the
first release that writes these forms, so before it there is no release to roll back to.

## Open questions

All answered as recommended: [RC020](DECISIONS.md#rc020--the-signed-formats-for-the-release-1-cutover) (Hongwei, 2026-11-20).

| # | Question | Recommendation |
|---|----------|----------------|
| Q1 | JSON objects: the extension area as `ext_b64` (the binary TLV, base64) or as a JSON map of entries? | **`ext_b64` TLV.** One encoding everywhere; the signed bytes are the decoded entries, so no JSON canonicalization (forbidden in signed bytes by D088); a JSON map's duplicate keys are read differently by different parsers, a known signature-bypass route |
| Q2 | Algorithm names: a one-byte id inside every key, signature, KEM and AEAD blob, or an `*_alg` field beside each? | **Typed blobs.** No new fields; the payload blob and `FileCipher` bytes are already of that shape; one helper decodes every blob |
| Q3 | Drop the `-vN` suffix from every signing domain at this cutover? | **Yes.** All signed bytes change anyway and release 1 freezes the names; otherwise `account-card-v4` carries 5 for good. If no: keep the strings, and the `u8` after them is the version |
| Q4 | One sign version (4) for every relay domain? | **Yes.** Three numbers today with no meaning between them; a later change to one domain bumps only that one |
| Q5 | The relay id's form: `relay:` + base64url(BLAKE2b-256 of the founding operator set), in cards, locators and links (`#relay:…`)? | **Yes.** Mirrors the Account ID; 49 characters keeps the locator small; a later algorithm gets a new prefix |
| Q6 | Does the certificate bind the relay's origins (1–4)? | **Yes.** It closes MR008 item 6 (a hostile relay fronting another); the cost is a new certificate when an origin changes, which RC018 already wants for a move |
| Q7 | Certificate expiry: RC018 part 2's default is none in release 1. Ship the `expires_at` field (0 = none) and its reader anyway? | **Yes, field and reader; operators write 0.** Leo can start expiry later without a format change; adding the field later would make every in-range app say "update needed" |
| Q8 | Operator-set changes (replace a lost or leaked operator key, keep the relay id): ship the reader in release 1? | **Yes, the reader; no use until its runbook.** Without it, losing one key of 2 of 3 means a new relay id for everyone |
| Q9 | Set sizes per environment: prod 2 of 3 (Leo's default); staging and dev? | **Target: staging and prod 2 of 3** (staging rehearses the ceremony and rotation); **dev and local 1 of 1**. **Current: dev, staging and prod all run 1 of 1** (the committed certificates say so); moving staging and prod to 2 of 3 is an operator-set change signed by the set in force, relay id unchanged (§ 3.2 / § 3.6), its runbook still pending (Q8). Holders and escrow: Leo |
| Q10 | Builds embed each environment's current certificate as a floor, beside its relay id? | **Yes.** No first fetch before the first signed call, and a fresh install never accepts an older certificate than its build knew |
| Q11 | During a planned rotation, the relay also accepts the previous transport PeerId as audience: for how long? | **24 h after the new certificate is deployed; never after a compromise** (Leo's runbook says which) |
| Q12 | Pre-release relay data release 1 cannot read (v4 cards, record-1 lockers, envelope-3 mailboxes): delete at the cutover, or keep a reader? | **Delete, with relay 1.1's deploy.** RC017's clean start; testers who restore keep their Account ID and re-add contacts (direct contact). A reader would be the first pre-release bridge in release 1 |
| Q13 | The relay's signing key: certified from release 1 but used for nothing yet. Leave it unused? | **Yes.** Certifying it now is what keeps later relay-issued items (signed answers, receipts) additive |
| Q14 | No extension area per envelope copy? | **None.** Per-device data belongs in the copy's ciphertext (ChatPayload, T4) |
| Q15 | The DHT record in this cutover (pp-cpp-mesh)? | **Yes.** pp-node 10.0.0 ships with release 1; V3 needs its version read first anyway |
| Q16 | Extension area cap? | **4 KiB per object**, the certificate included; a reader refuses more as malformed |

Not reopened here, decided by RC018 item 7: the transport key stays the audience (not the relay id), and clients
pin the operator set (as its relay id), not the PeerId.
