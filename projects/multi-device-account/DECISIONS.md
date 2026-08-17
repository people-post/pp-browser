# Decisions — multi-device account

Format: **ID**, **date**, **decision**, **rationale**, **alternatives**.

Cross-project: [chat-storage D099](../chat-storage-and-memory/DECISIONS.md#d099--account-id-amends-d096-multi-device), [e2e E025](../e2e-message-crypto/DECISIONS.md#e025--account-envelope-signing--private-psk-not-auto-synced), [at-rest A010](../at-rest-crypto/DECISIONS.md#a010--shared-dek-per-device-vault-wrap-multi-device).

---

## M001 — Account ID as person root; Peer ID per device; relay as route

**Date:** 2026-08-11  
**Decision:** Introduce a portable **Account ID** as the account/person identity. **Peer ID** is **per device** (endpoint). **`relay_user_id`** is a **per-server route binding**, not the person root. **CAIP-10** remains find/attest only (D091).  
**Rationale:** Enables multi-relay under one person; avoids libp2p Peer ID conflicts when two installs are online; matches pre-release willingness for a hard cut.  
**Alternatives:** Account = `relay:` only (rejected — provider-bound); account = Peer ID (rejected — multi-device clash); account = CAIP-10 (rejected — optional, not wire).

---

## M002 — Account ID format: PQ hash-binding of ML-DSA-65 pubkey

**Date:** 2026-08-11  
**Updated:** 2026-08-11 — aggressive PQ: Account ID no longer Ed25519-derived.  
**Decision:**

```text
account:<base64url-unpadded(BLAKE2b-256(ML-DSA-65 account public key))>
```

Directory / identity publish the full **1952-byte ML-DSA-65** public key; Account ID **must** equal the hash binding. UTF-8 exact, case-sensitive, no trim.  
**Rationale:** PQ-rooted person id without putting ~2.6k chars on every wire field; forgery still requires ML-DSA secret key.  
**Alternatives:** Full ML-DSA pubkey in id (accepted if desired later); Ed25519-derived id (rejected for account root); random UUID (not crypto-bound).

---

## M003 — Account ML-DSA-65 signs envelopes; device ML-DSA-65 is endpoint-only

**Date:** 2026-08-11  
**Updated:** 2026-08-11 — aggressive PQ: account signing is **ML-DSA-65 only** (not Ed25519 hybrid).  
**Updated:** 2026-08-13 — dogfood is **one active sender** across linked devices (**M016**); `sender_instance_id` (D074) still later.  
**Updated:** 2026-08-17 — [libp2p-pq-transport P004](../libp2p-pq-transport/DECISIONS.md#p004--hard-cut--wipe-amend-m003m008e025): device endpoint key is **ML-DSA-65** (not Ed25519).  
**Decision:** **Account** key = **ML-DSA-65** (vendored `mldsa-native`) signs all relay envelopes. **Device** ML-DSA-65 keypair derives Peer ID / libp2p Noise identity only — not envelope `signature`. Installs distinguished by `sender_instance_id` when multi-writer ships (D074).  
**Rationale:** Maximize PQ for person-level auth and mesh endpoint identity.  
**Alternatives:** Ed25519+ML-DSA hybrid (S1 classic); device signs + account attests (S2); classical Ed25519 Peer ID (rejected — P004).

---

## M004 — Shared DEK; per-device vault wrap

**Date:** 2026-08-11  
**Decision:** Linked devices share one **DEK**. Each install has its own **`vault.bin`** (PIN-derived wrap; PIN may differ). Link seals DEK to the new device; new device wraps into its vault.  
**Rationale:** Minimizes shared on-disk vault material while keeping one secrets realm; matches at-rest A001 layering (PIN wraps DEK).  
**Alternatives:** Different DEK per device (heavier re-seal); clone identical `vault.bin` (same PIN everywhere).

---

## M005 — Private PSKs not auto-synced; public(/group) may sync

**Date:** 2026-08-11  
**Updated:** 2026-08-13 — public auto-key uses **account KEM** (copied on link — **M015**); private PSK rule unchanged.  
**Updated:** 2026-08-15 — only `key_scope=account` public PSKs sync on link (**M020**).  
**Decision:** **Private (`e2e`) `master_psk` / retired ledger are not auto-synced** to linked devices. Public (`e2e_public`) and group pair keys **may** sync with account/DEK when `key_scope=account` ([M020](#m020--device-scoped-public-psks-stay-off-the-link-bundle)). Directory encapsulate-to is the **account** ML-KEM-768, not a per-device key (**M015**). Body encryption remains PSK AEAD on all tiers — “device-bound private” means **which devices hold the PSK**, not a different cipher.  
**Rationale:** Preserves private-tier assurance under account signing (S1); stolen/linked laptop does not silently receive every private chat key.  
**Alternatives:** Sync all PSKs with DEK; device-signed private envelopes only (S3).

---

## M006 — Brief register binding: one relay id per Account ID per server

**Date:** 2026-08-11  
**Decision:** Registration proves the **account** key. Each Brief-compatible server maintains **at most one** active `relay_user_id` per Account ID. Other servers may bind the same Account ID to different route ids later.  
**Rationale:** Portable account with provider-local mailboxes; devices attach under the binding rather than re-registering as new people.  
**Alternatives:** Keep register tied to device/Peer ID key only; multiple relay ids per account on one server.

---

## M007 — Pre-release hard cut: communicating identity → Account ID

**Date:** 2026-08-11  
**Decision:** Target wire and `ChatTargetKey` use **Account ID** as communicating identity (destructive OK pre-release). `relay:` remains route/binding. Implementation timing in [PHASES.md](PHASES.md) m1–m2.  
**Rationale:** Avoid shipping `relay:`-as-person then migrating again.  
**Alternatives:** Soft cut (Account ID for link/DEK only; wire stays `relay:`) — faster but weak multi-relay threads.

---

## M008 — PQ libraries and KEM: mlkem-native + mldsa-native; ML-KEM-768 only

**Date:** 2026-08-11  
**Updated:** 2026-08-17 — libp2p Noise/PeerId also use mlkem-native / mldsa-native ([libp2p-pq-transport](../libp2p-pq-transport/)).  
**Decision:** Vendor **mlkem-native v2.0.0** (ML-KEM-768, C backend) and **mldsa-native v2.0.0** (ML-DSA-65, C backend) under `third_party/`. Public auto-key uses **ML-KEM-768 only** (no X25519 hybrid). App wrappers: `HybridKem` (ML-KEM-768; name retained) and `MlDsa`. Symmetric stack remains libsodium. Libp2p product path: device PeerId/Noise auth = ML-DSA-65; Noise secrecy = ML-KEM-768 (`/noise-mlkem768/1.0.0`). BoringSSL remains for HTTPS/curl and classical crypto still present in the fork for non-product paths.  
**Rationale:** High-assurance PQCP implementations; matches aggressive PQ account + mesh model; Brief updates KEM blob size to 1184 in parallel.  
**Alternatives:** liboqs umbrella; keep X25519+Kyber-draft BoringSSL experimental path; classical Noise (rejected — P002).

---

## M009 — ContactIdKind::Account; wire `peer_identity_kind=account`

**Date:** 2026-08-13  
**Implements:** [M007](#m007--pre-release-hard-cut-communicating-identity--account-id).  
**Cross-project:** [chat-storage D099](../chat-storage-and-memory/DECISIONS.md#d099--account-id-amends-d096-multi-device), [D100](../chat-storage-and-memory/DECISIONS.md#d100--release-scope-b-pq-account-id).  
**Decision:**

1. Add **`ContactIdKind::Account`** with wire/string kind **`account`**.
2. Value is the full Account ID string (`account:<base64url-unpadded(BLAKE2b-256(ML-DSA-65 pk))>` — M002). UTF-8 exact, case-sensitive, no trim.
3. On contacts, **Account** is the **primary** person id. **`relay_user`** and **`peer_id`** remain secondary (route / endpoint).
4. Do **not** overload `Custom` or store `account:…` under `relay_user`.

**Rationale:** Matches D079 kind/value model; one communicating identity for multi-device and multi-relay without a second migration.  
**Alternatives:** Soft dual wire (`relay:` person + Account ID) — rejected (M007); Peer ID as wire person — rejected (multi-device clash).

---

## M010 — Envelope/AAD = Account ID; relay API auth stays `relay:`

**Date:** 2026-08-13  
**Decision:**

| Surface | Identity |
|---------|----------|
| Envelope `sender_contact_id`, `ChatTargetKey`, E2E AAD | **Account ID** (`peer_identity_kind=account`) |
| Peer signing-key cache / resolver key | **Account ID** |
| Relay inbox / API auth (`requester`, send parties as route handles, device register) | **`relay:`** (M006 binding) |
| Optional `sender_relay_id` (if present) | Route metadata only — not communicating identity |

Ingest verifies ML-DSA against the Account ID’s published pubkey (directory/cache); Account ID **must** equal hash(pk) (M002).

**Rationale:** Person vs route split (M001); keeps Brief delivery queue keyed by existing relay binding while threads/PSK state key on portable Account ID.  
**Alternatives:** Switch inbox auth to Account ID (heavier API churn, no multi-relay gain yet); keep `relay:` on envelopes (rejects M007).

---

## M011 — Brief directory Account-first; by-account lookup; search `q=` matches Account ID

**Date:** 2026-08-13  
**Updated:** 2026-08-13 — **M017** replaces the single-`peer_id` allowance (m3).  
**Ship order:** Brief (www) API **before or in the same window as** client m2 — not a post-m2 polish.  
**Decision:**

1. **Search** `GET /v1/search?q=`: hits include top-level **`account_id`**; `ids[]` lists **`account` as primary**, then `relay_user` / `peer_id` as secondary. Query matches **nickname**, **`relay:`** id, and **Account ID** (including prefix match on the `account:…` value).
2. **New** `GET /v1/users/by-account/:account_id` — person lookup returning signing/KEM keys, `relay_user_id`, `signature_alg`, nickname, **`endpoints[]`** ([M017](#m017--directory-endpoints-per-device-no-last-write-wins)).
3. Keep `GET /v1/users/:relay_user_id` as **route** lookup; response **must** include `account_id`.
4. Register finish may echo `account_id`. **Amended by [M017](#m017--directory-endpoints-per-device-no-last-write-wins):** upsert `endpoints[]` by this device’s `peer_id`; directory responses do not emit top-level `peer_id` / `multiaddrs`.

**Rationale:** Client m2 can resolve signing keys and add-contact by Account ID without a temporary “relay lookup then read account_id” shim.  
**Alternatives:** Defer by-account until after wire cut (rejected — forces dual-path client); drop route lookup by `relay:` (rejected — inbox still uses relay ids).

---

## M012 — Link-device ritual (deferred until m4)

**Date:** 2026-08-13  
**Updated:** 2026-08-13 — first-run identity fork; Security is export-only.  
**Updated:** 2026-08-13 — paste snapshot of contacts + public thread index (**M018**); unlink spec (**M019**).  
**Status:** **Accepted** (m4b paste path; QR later).  
**Decision:**

1. Transport this pass: **paste payload** (QR primary still later; short code fallback later).
2. Seal to new device: Account ID + account ML-DSA secret + **account ML-KEM-768** + DEK + public(/group) PSKs only — **never** private `e2e` PSKs (M005 / M015). Keep per-device ML-DSA-65 / Peer ID. **m4c ([M018](#m018--link-paste-includes-contacts--public-thread-index)):** also contacts + public thread *index* (not transcripts).
3. Old device: unlocked + explicit confirm; **Copy link payload…** on Me → Security (registered). New device: first secrets use shows **I'm new on this device** vs **I already have an account**. Link path: PIN for *this* device, then paste into an **empty vault** (`CreateWithDek`). Do not mint a Brief person on the new install first. If the profile is already a person, **Reset this profile** then the same fork — no in-place Security join, no account switcher.
4. **Per-device inbox cursor** from day one (shared watermark would starve siblings).
5. Unlink/revoke: **[M019](#m019--unlink-local-forget-kem-rotation-is-revoke)** — spec now; local-forget after m3; KEM rotation later. Do not claim a stolen device loses public/group keys until rotation ships.

**Rationale:** Joining an existing account must not create a throwaway person or graft onto a vault that already wraps a different DEK. Export stays on the used device; import belongs with first secrets use.  
**Alternatives:** In-place Me → Security import (rejected this pass); shared inbox watermark (rejected); auto-sync all PSKs (rejected — M005).

---

## M013 — Soft inbox ack (shared mailbox; no sibling starve)

**Date:** 2026-08-13  
**Status:** Accepted (m4a).  
**Implements:** [M012](#m012--link-device-ritual-deferred-until-m4) §4 (per-device progress) without server `device_id` on poll yet.  

**Decision:**

1. One Account → one `relay_user_id` mailbox (M006). Each device keeps its own local poll cursor (`relay_inbox_cursor.json` per profile).
2. **`POST /v1/inbox/ack` is soft:** validate cursor; return `{deleted:0}`; **do not** `deleteMany` through the watermark.
3. Mailbox GC: **90-day TTL** on `relay_messages.created_at` (startup rewrites the TTL index only if `expireAfterSeconds` differs), **soft per-recipient FIFO cap** (trim toward **1000** when over **~1200**; sampled on send), plus account-wide **`/inbox/clear`** (manual recovery). Document that `clear` affects all siblings.
4. Clients may keep calling ack after ingest (no API break); `deleted` may be 0.

**Follow-up (not m4a):** optional server table of `(relay_user_id, device_id, cursor)` and GC when **min** watermark across registered push devices advances.

**Rationale:** Destructive ack was the only starve point; local cursors were already per-device. Soft-ack unblocks link-device without fan-out copies or signed `device_id` churn.  
**Alternatives:** Fan-out N rows per device (rejected — cost); require `device_id` on poll day one (deferred); stop calling ack on client only (rejected — leaves misleading “delete” semantics on older messengers).

---

## M014 — Private Secure: one session per pair; transferable lock; not device-keyed

**Date:** 2026-08-13  
**Status:** Accepted (policy for m4b+).  
**Updated:** 2026-08-13 — dogfood one active sender (**M016**); transfer UX still later.  
**Amends:** [M005](#m005--private-psks-not-auto-synced-publicgroup-may-sync) (UX/isolation, not the no-auto-sync rule).

**Decision:**

1. Private (`e2e`) stays **one `ChatTargetKey` per pair**: Account ID + channel `e2e`. Do **not** put Peer ID / device id on `ChatTargetKey` or the wire.
2. Link-device does **not** copy private PSKs (M005). “Add this device” / transfer = existing PSK bundle plus seq/epoch (same fingerprint; peer does not re-verify).
3. A linked install must **not** start a second independent Secure PSK with someone who already has a private session on another of the user’s devices (forks seq/AAD and can latch compromised).
4. Two devices **both sending** on the same Secure lock is **D074** (`sender_instance_id`), not a new thread id. Dogfood until then: **one active sender** (**M016**).
5. Optional later: `secure_session_id` on `route` only if the product wants two concurrent Secure drawers with the same person.

**Rationale:** Person stays Account ID; the lock is portable secrets + seq, not a radio identity.  
**Alternatives:** Device-keyed private threads (rejected — transfer breaks identity); auto-sync all private PSKs (rejected — M005).

---

## M015 — Account KEM for public/group auto-key; private e2e stays device-local

**Date:** 2026-08-13  
**Updated:** 2026-08-13 — unlink vs KEM rotation (**M019**); paste contacts/index is not live sibling sync (**M018**).  
**Updated:** 2026-08-15 — device-scoped public PSKs stay off the link bundle (**M020**).  
**Amends:** [M012](#m012--link-device-ritual-deferred-until-m4) (bundle includes account KEM); [E024](../e2e-message-crypto/DECISIONS.md#e024--auto-key-trust-anchor-for-e2e_public-o007) (encapsulate-to is the person).  
**Decision:**

1. Directory `kem_public_key_b64` is the **account** ML-KEM-768 public key (same person on every linked device). Register / `GET /v1/users` publish that key. Link-device copies `account_kem_pk_b64` / `account_kem_sk_b64` with account ML-DSA.
2. Public (`e2e_public`) auto-key and later **group pairwise** encapsulate **once** to that account KEM so every linked install can open `key_init` from the shared mailbox.
3. Each public conversation still has **one `master_psk` per `ChatTargetKey`** (not one global account PSK). Linked devices **share** that conversation key when `key_scope=account` (link snapshot today; sibling refresh later). Public **does not auto-`rotate_psk`** on account scope; explicit device-lock and D2D auto-rekey are **[E027](../e2e-message-crypto/DECISIONS.md#e027--public-11-device-lock-rekey-auto-rotate_psk-only-when-both-sides-are-device-bound)** / **[M020](#m020--device-scoped-public-psks-stay-off-the-link-bundle)**.
4. **Private (`e2e`)** stays device-local: no private PSK in the link bundle (M005 / M014); no account-KEM handshake for Secure.
5. Device Ed25519 / Peer ID stay per install (dial). Unlink does not revoke account KEM; rotating account KEM is the revoke story (**M019**).
6. **Deferred:** sibling public-PSK + chat-index *refresh* when both devices are reachable; optional relay pin of inbound `key_init`. Do not promise “connect to your other device” until that sync exists. Link payload remains one-shot empty-vault import — not an incremental refresh. **m4c** may snapshot contacts + public thread index in that one-shot (**M018**); that is not live sync.

**Rationale:** Messages to the person should be readable on every linked device for public/group; private Secure keeps a stolen laptop from learning those PSKs.  
**Alternatives:** Per-device KEM + N `key_init`s (rejected — senders must know every laptop; late-linked devices still miss old handshakes); cloud message sync (rejected — D100).

---

## M016 — Dogfood: one active sender on linked devices

**Date:** 2026-08-13  
**Status:** Accepted (dogfood until D074).  
**Amends:** [D015](../chat-storage-and-memory/DECISIONS.md#d015--single-active-sender-per-identity-v1) (two *devices* are allowed; two *senders* are not); [M014](#m014--private-secure-one-session-per-pair-transferable-lock-not-device-keyed) §4.

**Decision:**

1. Linked installs of the same Account ID are **in scope** for dogfood. **Only one should send** at a time (any channel). Receive, inbox poll, and push on every linked device are fine.
2. Conflicting `sender_seq` stays a **soft integrity failure** (D011 / D038) — pause + choice sheet, not silent merge.
3. **D074** (`sender_instance_id` + `envelope_version`) remains the dual-writer protocol. Do not ship it in m3 / m4c.
4. Me → Security help next to **Copy link payload…** must say that linked devices share the account and that send should stay on one device for now.

**Rationale:** Paste-link already creates a second Peer ID; seq collision is the remaining hazard. Documenting one sender unblocks dogfood without a wire bump.  
**Alternatives:** Ship D074 before encouraging two devices (rejected — heavier than this slice); ban send on the newly linked install (rejected — odd product); treat two senders as supported (rejected — D015 still true).

---

## M017 — Directory `endpoints[]` per device; no last-write-wins

**Date:** 2026-08-13  
**Updated:** 2026-08-13 — hard cut: directory responses are `endpoints[]` only (no top-level `peer_id`).  
**Status:** Accepted (m3).  
**Amends:** [M011](#m011--brief-directory-account-first-by-account-lookup-search-q-matches-account-id) §4.  
**Ship order:** Brief (www) **before or with** the client — same rule as M011.

**Decision:**

1. Each Account ID on a Brief server has **`endpoints[]`**: `{ peer_id, multiaddrs[], updated_at }` (libp2p Peer ID per install). Push `device_id` stays a separate table (M013 / push-notifications).
2. **`POST /v1/register/finish`** (and renew) **upserts** this device’s row by `peer_id`. It must **not** delete or overwrite sibling rows. Omitting `peer_id` leaves `endpoints[]` unchanged.
3. **`GET /v1/users/by-account/:account_id`**, **`GET /v1/users/:relay_user_id`**, and search hits **return `endpoints[]`**. Do **not** emit top-level `peer_id` / `multiaddrs` (pre-release hard cut). `ids[]` still lists each endpoint Peer ID as secondary.
4. Client dial: LAN / already-open session if present; else newest `updated_at`; else first endpoint. Call multi-ring across endpoints is later.
5. Promote the shipped shape into [SERVICE_ENDPOINTS.md](../../docs/contracts/SERVICE_ENDPOINTS.md) in the same window as m3.

**Rationale:** A second linked device today last-write-wins the directory `peer_id`, so dial and calls hit the wrong install. Inbox still works because `relay:` is shared; mesh identity must list every Peer ID.  
**Alternatives:** Stop publishing Peer ID from the secondary until this ships (stopgap only); last-write-wins forever (rejected — second device is not dialable); require `device_id` on directory rows (rejected — mix push ids with libp2p).

---

## M018 — Link paste includes contacts + public thread index

**Date:** 2026-08-13  
**Status:** Accepted (m4c).  
**Amends:** [M012](#m012--link-device-ritual-deferred-until-m4) §2. Does **not** amend [M015](#m015--account-kem-for-publicgroup-auto-key-private-e2e-stays-device-local) live sibling refresh (still later).

**Decision:**

1. Extend `pp-browser-link-device-v1` with:
   - **`contacts[]`** — address-book entries (`ContactToJson` shape: local display name / trust + remote ids / multiaddrs). Merge on import by primary Account ID.
   - **`public_threads[]`** — catalog rows only: `peer_identity_kind` / `peer_identity_value` / `channel=e2e_public` / `title`. Import calls `FindOrCreateDirectThread`. **No messages.**
2. Still **never**: private `e2e` PSKs, private thread rows, transcripts, AI threads, group rows until group keys ship.
3. Payload stays one-shot empty-vault import (M012 / M015). Raise `kMaxLinkDeviceBundleBytes` if needed; if still over cap, drop `public_threads[]` before `contacts[]`.
4. New-device paste UI shows the **Account ID** (truncation OK) for a visual check before import (DESIGN ceremony).

**Rationale:** Keys-only import leaves the new device blank even though public PSKs arrived. Contacts + public index make chats openable; history still comes from the shared mailbox and later fetch — not a second paste and not cloud CRDT (D100).  
**Alternatives:** Keys only until live sibling sync (rejected — unusable dogfood); put transcripts in the paste (rejected — size, TTL, D100); include private thread shells (rejected — invites opening Secure without a PSK).

---

## M019 — Unlink: local forget; KEM rotation is revoke

**Date:** 2026-08-13  
**Status:** Accepted (spec now; **ship after m3**).  
**Amends:** [M012](#m012--link-device-ritual-deferred-until-m4) §5; [M015](#m015--account-kem-for-publicgroup-auto-key-private-e2e-stays-device-local) §5.

**Decision:**

1. **Unlink does not drop account KEM** (M015). Settings copy must not say a stolen device loses public/group keys until phase 2.
2. **Phase 1 (after m3):** this install forgets the account (reset profile / drop identity) and **unregisters push** for its `device_id`. Brief `endpoints[]` should drop that `peer_id` when the client can sign a remove; remaining devices keep the Account ID + KEM + mailbox. Device list on Me → Security can appear once M017 exists.
3. **Phase 2 (recovery, later):** **rotate account KEM**, remaining devices receive the new secret (re-link or sibling refresh), stolen device cannot open new public/group `key_init`. That is revoke for public/group. Private `e2e` PSKs were never on the stolen laptop via link (M005) unless that install created them.
4. Account **ML-DSA / Account ID** rotation is a different product (new person) — still open. Remote wipe / cloud vault stay **D100** deferred.

**Rationale:** A “Unlink…” control that only deletes local files would over-promise. Naming KEM rotation as revoke keeps M015 honest and unblocks a truthful phase-1 control after directory endpoints exist.  
**Alternatives:** Local-forget only with no rotation story (rejected — stolen laptop keeps account KEM); remote wipe as unlink (rejected — D100); rotate Account ID on unlink (rejected — breaks every contact’s person key).

---

## M020 — Device-scoped public PSKs stay off the link bundle

**Date:** 2026-08-15  
**Status:** Accepted.  
**Amends:** [M015](#m015--account-kem-for-publicgroup-auto-key-private-e2e-stays-device-local) §3; [M005](#m005--private-psks-not-auto-synced-publicgroup-may-sync); [M012](#m012--link-device-ritual-deferred-until-m4) `public_psks[]`.  
**Cross-project:** [E027](../e2e-message-crypto/DECISIONS.md#e027--public-11-device-lock-rekey-auto-rotate_psk-only-when-both-sides-are-device-bound), [D101](../chat-storage-and-memory/DECISIONS.md#d101--public-key_scope-psk_rotate-ingest-and-rotation-policy).

**Decision:**

1. Directory / first-message public auto-key remains the **account** ML-KEM-768 (M015). Group pairwise encapsulate-to stays account KEM. Private `e2e` unchanged.
2. After **E027** device-lock, that conversation’s `master_psk` (and conversation KEM secret) MUST **not** appear in `public_psks[]` on `pp-browser-link-device-v1`. `LinkDeviceCoordinator::CollectPublicPsks` copies only rows with `key_scope=account`.
3. A newly linked install that still holds account KEM can open **account-scope** `key_init`s from the mailbox. It cannot open `wrap_kind=thread_kem` blobs, and it must not receive device-scoped PSKs via paste.
4. Conversation KEM secrets are install-local (DEK-wrapped on `chat_targets`), never in the link bundle.

**Rationale:** Link-device is how public chats follow the person. Once the user opts a thread onto one device, copying that PSK would undo the lock.  
**Alternatives:** Copy all public PSKs then mark locked-out on the new device (rejected — new device would briefly hold the key); include conversation KEM sk in the bundle (rejected — that is the lock).
