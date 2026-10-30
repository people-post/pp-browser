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
**Superseded:** by [M021](#m021--per-device-sessions-a-master-device-keeps-the-device-list-stated-limits) from d2 — each device signs its envelopes; the account key signs the device list (the card).  
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
**Superseded:** §2 by [M021](#m021--per-device-sessions-a-master-device-keeps-the-device-list-stated-limits) — no public sessions in the bundle from d2; no account signing key from d4 (the master signs the new device in).  
**Superseded:** the m4c part of §2 (contacts + thread index in the paste, M018) by [M023](#m023--account-state-a-recovery-phrase-is-the-root-an-encrypted-account-locker-any-provider-can-hold) — a linked device reads account state from the locker.  
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
**Superseded:** §2–3 by [M021](#m021--per-device-sessions-a-master-device-keeps-the-device-list-stated-limits) from d2 — sessions are per device pair, opened to the device KEM; the account KEM only opens a first contact (account copy).  
**Superseded:** §6 (sibling chat-index refresh) by [M023](#m023--account-state-a-recovery-phrase-is-the-root-an-encrypted-account-locker-any-provider-can-hold) §7 — the change log; sessions are not synced at all (M021).  
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
**Superseded:** by [M021](#m021--per-device-sessions-a-master-device-keeps-the-device-list-stated-limits) from d2 — numbering is per device pair, so every device may send.  
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
**Superseded:** by [M023](#m023--account-state-a-recovery-phrase-is-the-root-an-encrypted-account-locker-any-provider-can-hold) — never built; a linked device reads account state from the locker and the change log. Do not implement m4c.  
**Status:** Accepted (m4c), then superseded before it shipped.  
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
**Superseded:** §3 by [M021](#m021--per-device-sessions-a-master-device-keeps-the-device-list-stated-limits) from d4 — revoke is the master signing a device list without the device.  
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
**Superseded:** by [M021](#m021--per-device-sessions-a-master-device-keeps-the-device-list-stated-limits) from d2 — the link bundle carries no sessions at all.  
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

---

## M021 — Per-device sessions; a master device keeps the device list; stated limits

**Date:** 2026-10-25  
**Status:** Accepted (Hongwei, 2026-10-25). Built in phases **d1–d6** ([PHASES.md](PHASES.md#d--per-device-sessions-m021)); until a phase ships, the decisions it supersedes still describe the code.  
**Amended:** §8 and limit 6 by [M023](#m023--account-state-a-recovery-phrase-is-the-root-an-encrypted-account-locker-any-provider-can-hold) — the account key's backup is the **recovery phrase** (not the local account backup, which is unsupported, at-rest A011); restoring from it still makes a new master that signs a fresh list. Account state (contacts, groups, …) comes from the M023 locker.  
**Supersedes (as its phases ship):** [M003](#m003--account-ml-dsa-65-signs-envelopes-device-ml-dsa-65-is-endpoint-only) (devices sign envelopes, d2), [M012](#m012--link-device-ritual-deferred-until-m4) §2 and [M020](#m020--device-scoped-public-psks-stay-off-the-link-bundle) (no sessions in the link bundle, d2; no account signing key, d4), [M015](#m015--account-kem-for-publicgroup-auto-key-private-e2e-stays-device-local) §2–3 (the account KEM only opens the first contact, d2), [M016](#m016--dogfood-one-active-sender-on-linked-devices) (every device may send, d2), [M019](#m019--unlink-local-forget-kem-rotation-is-revoke) §3 (revoke = a device list without it, d4), [D074](../chat-storage-and-memory/DECISIONS.md#d074--multi-device-extension-point-sender_instance_id) and [D015](../chat-storage-and-memory/DECISIONS.md#d015--single-active-sender-per-identity-v1) (no `sender_instance_id`; the device is in the pair, d2). Leaves [M014](#m014--private-secure-one-session-per-pair-transferable-lock-not-device-keyed) (private Secure stays on one device) as it is. [E027](../e2e-message-crypto/DECISIONS.md#e027--public-11-device-lock-rekey-auto-rotate_psk-only-when-both-sides-are-device-bound) device-lock: to be re-decided on top of this (d6).

**Principle:** there is no perfect multi-device design without a server that knows everything. This one keeps every rule local and simple, and **states its limits** (below) instead of trying to close every case.

**Decision:**

1. **The device is the endpoint.** A device's id is its Peer ID (its own ML-DSA key, M001). Each device also has its **own ML-KEM key**. Both stay on the device: never in a link bundle or a backup.
2. **Device list.** The account card lists the account's devices: each entry is the Peer ID, the device signing key and the device KEM key. It is signed by the **account** key and versioned by `issued_at`; **the newest card wins** (no union of lists). Only a device holding the account key issues one — today every linked device; from d4 only the **master** (below).
3. **Sessions per device pair.** An `e2e_public` session is between one of our devices and one of the peer's: the session key, its epoch and the numbering (`sender_seq`) are the pair's. A message is one copy per recipient device, each a 1:1 message in its device pair. Receivers tell senders apart per (thread, sender device, epoch). Two devices of one account never share a count, so there is nothing to collide (M016's limit goes away) and nothing to keep in sync between siblings.
4. **Groups inherit it.** A group copy is a pair message (G010/G011); the pair is now a device pair: one copy per member device, numbered in that device pair.
5. **Fan-out.** A send goes to every device on the peer's newest card we hold. With no card for the peer yet (first contact), one **account copy** goes to the account KEM, as today; it carries our card, and the replies (per device) carry theirs. From d3 a send also goes to our own siblings (what we sent shows on our other devices).
6. **Cards travel with sessions.** Every session opener (key_init) carries the sender's card, so a receiver always learns a new sender device from its first message. A newly linked device tells its siblings its card through the shared mailbox.
7. **Envelopes.** Each names its sender device and its recipient device (empty for an account copy), both signed, and is signed by the **sender device's** key, checked against the sender's card. A device skips copies for its siblings in the shared mailbox (M013).
8. **Master device (d4).** One device per account holds the account signing key — by default the first; the user can move the role while both devices are online. The master signs new devices into the list (link becomes two-step: the new device shows its keys, the master signs it in) and takes devices out (revoke). The backup holds the account key: **restoring makes a new master**, which signs a fresh list holding only itself; the other devices re-link. An old master that sees a newer list without itself stops sending. Its other job is to be the device most likely to have everything — not a hub messages go through.
9. **Missing copies (d3).** A device that finds a message in the shared mailbox with copies for its siblings but none for itself asks the sender's device, which still holds the plaintext, to send it one (its card goes with the ask).
10. **Cap:** 5 devices per account (fan-out cost grows with devices × members).

**Limits (stated, not designed away):**

1. A newly linked device has nothing from before it joined (history recovery is later).
2. A message reaches the devices on the sender's view of the recipient's list. A device the sender did not know yet (just linked, just restored) gets it only through the ask in 9 — so it is **lost for good** when the sending device is gone or reset before the ask reaches it. Until d3 it is not asked for at all.
3. A device offline longer than the relay keeps its mailbox (90 days, M013) loses its own copies; siblings do not fill gaps (later, with history recovery).
4. Revoking is eventual: a removed or stolen device keeps receiving from peers that have not fetched the newer list, and keeps what it already has. From d4 it cannot add itself back (no account key). Until d4 every linked device holds the account key, so removing one is not a revoke.
5. The account KEM still opens first contacts (account copies), and every linked device holds it: a stolen device reads first-contact messages until the account KEM rotates (M019 phase 2).
6. Master lost without a backup: the device list is frozen — the other devices keep working; none can be added or removed. With the backup: restore, which signs out every other device.
7. A master that hands its role on is trusted to delete the account key; that cannot be proven.
8. Up to 5 devices; a message costs one copy per recipient device (× members in a group).
9. Read state and deletes do not sync between siblings yet.
10. Calls ring every device and the first to answer wins; the others may ring a moment longer (d5).

**Rationale:** Shared account sessions break as soon as two devices act: they collide on numbering, and any session one device opens (first contact, a restart, a peer's restart) is unknown to its siblings, so their copies of the replies fail. Syncing sessions between siblings races the mailbox. Per-device sessions remove the shared state instead of syncing it, and turn restore and new devices into the ordinary "a new device appears" case.

**Alternatives:** note-to-self session sync between siblings (rejected — races the mailbox, conflict rules on top of a fragile base); `sender_instance_id` on shared sessions (D074 — fixes numbering only, not session divergence); a sync hub master that relays messages (rejected — single point of failure; fan-out already reaches every device); every linked device holding the account key forever (rejected at d4 — no real revoke).

---

## M022 — One relay record per account carries every device's copy; an account writes to itself

**Date:** 2026-10-25  
**Status:** Accepted (Hongwei, 2026-10-25): split delivery; the mailbox cap raised. Amends [M021](#m021--per-device-sessions-a-master-device-keeps-the-device-list-stated-limits) §5, §7, §9 (d2/d3) and [M013](#m013--soft-inbox-ack-shared-mailbox-no-sibling-starve) §3 (the cap).

**Problem:** with per-device sessions (M021) a message to an account is one copy per device. As separate relay records each device downloads all of them anyway (one mailbox per account), the relay refuses an account's records to itself (no sibling channel: card notices, sent messages on our other devices), and a device that is not addressed cannot tell a copy is missing.

**Decision:**

1. **The relay stays account-keyed and knows no device list.** A send to an account is **one record**: one envelope holding an entry per recipient device — its device id, its `session_epoch` and `sender_seq` in that device pair, its session opener (`key_init`) if any, its payload — and one signature by the sending device over the envelope and every entry. An account copy (M021: no device known yet) is the entry with no device.
2. **An account writes to itself.** Each send also goes, as one record to our own account, with entries for our other devices: what we sent shows on them (d3), and a device's new card reaches its siblings through the mailbox. The relay allows a record whose sender and recipient are the same account (and its stream's history).
3. **Split delivery.** A device we have a live link to gets its entry over the mesh (Amp, a one-entry envelope); all the others share the relay record; an entry whose mesh send fails follows in another relay record. Messages stay relay-optional where devices are reachable, and no device is left out because a sibling was.
4. **A missing copy shows.** A device that finds a record with entries for its siblings but none for itself knows it is missing one (the ask of M021 §9).
5. **Mailbox cap doubled** (trim toward 2000 when past ~2400): our own records land in our mailbox too. TTL stays 90 days. The relay does not trim per device (it does not know devices).

**Limits:** a record's size shows roughly how many devices an account has (device PeerIds are public in the directory already, M017); every device downloads every entry of its account's records (at most 5×); the mailbox is trimmed by count and age, not by what each device has read; a device offline past either loses what was trimmed (M021 limit 3).

**Later (d4):** the relay authenticates requests with the account key; once only the master holds it, the relay accepts a device key with the account card listing it. Separate from this.

**Alternatives:** device-addressed mailbox (the relay keeps `recipient_device`, a device polls its own queue — less download, but the relay holds the device list and a device cannot see a missing copy; rejected); the relay fans one upload out per device (rejected — complicates signing for little at 5 devices); every message always through the relay, the mesh only a speed-up (rejected — relay-optional where peers reach each other); siblings only over the mesh (built, rejected — needs both online and reachable, and the privacy gate does not know siblings).

---

## M023 — Account state: a recovery phrase is the root; an encrypted account locker any provider can hold

**Date:** 2026-10-30  
**Status:** Accepted (Hongwei, 2026-10-30) — **design; not built** (phases **s1–s6**, [PHASES.md](PHASES.md#s--account-state-m023)). Until a phase ships the code is as the entries it amends describe: restore is unsupported ([at-rest A011](../at-rest-crypto/DECISIONS.md#a011--local-account-backup-and-restore-unsupported-in-the-product)) and nothing syncs account state between siblings.  
**Amends (as its phases ship):** [M021](#m021--per-device-sessions-a-master-device-keeps-the-device-list-stated-limits) §8 and limit 6 (the account key's backup is the recovery phrase, not the local account backup); [M015](#m015--account-kem-for-publicgroup-auto-key-private-e2e-stays-device-local) §6 (the deferred sibling chat-index refresh is the change log below).  
**Supersedes:** [M018](#m018--link-paste-includes-contacts--public-thread-index) (never built — a linked device reads account state from the locker, the link bundle carries no contacts or thread index); the local account backup ([DATA_LAYOUT § Local account backup](../../docs/contracts/DATA_LAYOUT.md#local-account-backup-ios-keychain-outside-the-data-dir), `LocalAccountBackup`) as the restore mechanism — its contents become locker snapshots and it is not re-enabled in the Keychain.  
**Cross-project:** [at-rest A010](../at-rest-crypto/DECISIONS.md#a010--shared-dek-per-device-vault-wrap-multi-device) §4 and [A011](../at-rest-crypto/DECISIONS.md#a011--local-account-backup-and-restore-unsupported-in-the-product); [group-chat G013](../group-chat/DECISIONS.md#g013--roster-sync-after-restore-group_roster_request--group_roster_reply) (groups ride in the locker).

**Problem:** with sessions per device pair (M021) nothing session-level needs syncing, but **account state** does — contacts, groups, card floors, preferences — and a device must find it when its siblings are offline or all gone (restore). Making a directory the agreed home for it hurts privacy and decentralization; the Keychain backup (A011) was device-local, PIN-guessable offline, and never covered a lost phone.

**Decision:**

1. **Three roles, kept apart.** *Trust* is the account key (self-certifying: Account ID = its hash, M002) — no server vouches for anything. *Availability* is an untrusted, encrypted **account locker** any provider can hold. *Live sync* is between online devices: Amp, and the account's own mailbox (M022).
2. **The root is a recovery seed**: 256 bits, shown as a 24-word recovery phrase. The account signing and KEM keys are derived from it deterministically, and so — under separate labels — are the locker id, the locker encryption key and the locker signing key. The seed and the account keys are **never stored on our servers** (relay, pp-node, any locker provider), PIN-wrapped or otherwise. Social recovery (shares held by contacts) may come later.
3. **Two recovery tiers, the user's choice** — like the vault's default PIN ("Just continue", at-rest A007), we serve those who do not care as well as those who do:
   - **Easy (default, "Just continue"):** the seed is kept in the platform's **synced** keychain — iCloud Keychain (`kSecAttrSynchronizable`) on Apple, Block Store on Android. No words to write; a new phone signed into the same platform account restores almost by itself. Whoever controls that platform account (and the device passcode) can take over the PP account — said in the UI, as plainly as the default PIN's caveat.
   - **Private:** the user writes the phrase down; nothing is kept in a synced keychain.
   - **Switching:** Me → Security shows the phrase at any time (behind the PIN), so an easy-tier user can write it down later; "Remove from iCloud / Google" deletes the synced copy and moves to private (only after the phrase was shown). Private → easy is one tap.
   - **Desktop** (Windows, Linux — no synced keychain): link from a phone (M021 d4), or write the phrase. No server-held escrow.
4. **What the locker holds:** account-level state only — contacts, groups (metadata, roster, the latest owner-signed statement, G012), card floors, the relay binding, the user's locker provider list, preferences that follow the person. **Never:** device keys or the device list's private parts (M021 §1), sessions (per device pair), private `e2e` material, messages.
5. **Snapshot format:** the state AEAD-encrypted under the locker key, padded to a size bucket, with a rising `version` (u64), signed by the locker signing key, stored under the locker id. A provider stores a snapshot only if it is signed by that locker's key and newer than the one it holds. A reader verifies, takes the **highest valid version across every provider it reaches**, and remembers the highest it has seen (never goes below it).
6. **Providers, any and several:** by default the relay the account is registered with (its blob store, under quota); the user can add their own pp-node, a friend's node, or an exported file. The provider list is part of the state; at restore the configured relay is tried first. No provider is authoritative.
7. **Live changes:** an encrypted change log to our own siblings — through our own mailbox (M022) and over Amp — applied per record, last write wins on a hybrid clock, with tombstones for removals. Group rosters are not merged: the owner's signed statement decides (G012). The snapshot is rewritten from the merged state, debounced.
8. **Restore** = seed (from the synced keychain on the easy tier, or the phrase typed in) → keys → locker → state. The restored install is a **new device** (M021): its own device keys, a fresh device list it signs as master (M021 §8), card floors applied before anything is received, then its card announced to every contact so their next message reaches it (built for the lab restore in #411: `AnnounceRestoredDevice`).
9. **Link** stays a device ritual (M021 d4: the master signs the new device in); the new device reads account state from the locker and the change log.

**Stated limits:**

1. Private tier: losing the phrase **and** every device loses the account. Easy tier: losing the platform account too. No server of ours can recover it.
2. Easy tier: the platform account (Apple / Google) is a way into the PP account.
3. A provider learns the locker id, the padded size and when it is written; with traffic analysis (IP, timing) it may link the locker to the account.
4. A device restored from nothing cannot detect rollback below the newest version among the providers it reaches.
5. Last write wins can drop one of two concurrent edits to the same record.
6. No message history: a restored or new device starts with none (peers may re-serve it — G013 amendment 3).

**Rationale:** Splitting trust from availability removes the need for a trusted home: the account key already proves everything, so the place that holds state only has to be reachable, and several untrusted places are better than one trusted one (availability, rollback resistance). A phrase keeps the root with the user instead of behind a PIN a server must rate-limit; the easy tier lends that root to the platform keychain the user already trusts with their passwords, by their choice, without making any of our servers a custodian. Sessions per device pair already made "a new device appears" the ordinary case, so restore needs only account state, not sessions.

**Alternatives:** a directory the user picks as the authority for account metadata (rejected — sees and controls it, central point); the PIN-wrapped Keychain backup (rejected — A011); PIN-wrapped keys on a server that limits guesses, or custodial escrow of the seed behind an email / password login (rejected — the provider could take over any account using it, a central honeypot; users are expected to have a phone or to be fine with the phrase or linking on desktop); the phrase wrapping a random account key stored in the locker (keeps today's random keys and allows key rotation, but puts the account key on servers, encrypted — revisit if account-key rotation needs it); one provider only (rejected — availability, rollback); message history in the locker (rejected for now — size, privacy).

**Open:** existing accounts' keys are random, not phrase-derived — pre-release, new accounts only ([COMPATIBILITY](../../docs/contracts/COMPATIBILITY.md) allows a wipe) unless decided otherwise in s1; the relay's locker API (relay change, merges and deploys first); tier UX copy (the easy tier's caveat; when to offer writing the phrase down).
