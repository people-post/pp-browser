# Social posts — distribution

**Status:** Draft (2026-10-23), for P2  
**Builds on:** [DESIGN.md](DESIGN.md) (log, heads, trust ladder) · decisions S017–S021 in [DECISIONS.md](DECISIONS.md)

How an author's posts reach readers who follow them, with no central server: where an author publishes,
the relay post API, fetching from the author's devices, head discovery, and the publish and refresh flows.
Everything served is checked by the reader (signatures, chains), so no carrier is trusted for content —
only for availability.

P2 is **fetch only** (S021): readers poll. Subscriptions (pushes) come after.

---

## 1. Where an author publishes

An author picks **publish targets** — services offering `post_relay` (S013). The default relay is one,
preselected.

- **Authoritative list:** the **head statement** carries the targets (S017): signed, newest wins, found
  wherever the log is. Up to 8 targets, each an `https://` base URL of at most 256 bytes.
- **Hints** (unsigned; the head decides): the invite locator gains an optional `posts` list of target URLs;
  a contact card may carry the sharer's known targets. A reader with no hint asks its own relays and the
  author's devices (§ 4).

Not in the account card: a card change is a new card version older clients reject, and the head already
travels with the log.

```text
head statement v1 (DESIGN § Head statement) gains, after the lanes:
u64 BE   target count (≤ 8), then per target: LenUtf8 url (https, ≤ 256 bytes)
```

---

## 2. Relay post API (`post_relay`)

HTTP under the target's base URL; JSON bodies; binary fields base64; failures as `{ "error": code,
"message" }`. The service is [app-posts](https://github.com/nfsc-brief-web3/app-posts) (deployable on its
own, base URL = its root); the default relay forwards `…/api/relay/v1/posts/*` to it, so the default
target is `https://www.brief.global/api/relay/v1`.

### Writes — authenticated by the author's own signatures (S018)

| Request | Body | Answer |
|---------|------|--------|
| `POST /posts/entries` | `{ "entries": [signed entry b64, …≤ 32], "key_card"?: {…} }` | per entry `{ "entry_hash", "status": "stored" \| "duplicate" \| "fork" \| "refused", "reason"? }` |
| `POST /posts/heads` | `{ "head": signed head b64, "key_card"?: {…} }` | `{ "status": "stored" \| "older" \| "refused" }` |
| `POST /posts/media/presign` | `{ "account_id", "content_id", "size", "mime", "issued_at", "signature" }` | `{ "upload_url", "public_url" }`, or `{ "present": true }` when held |

- An entry or head **is** its own authentication: the relay verifies the signature against the author's
  key (from its directory, or the `key_card` sent along — checked against the account id like any card).
  Republishing someone else's entries is allowed (mirroring) and idempotent.
- The media presign is signed by the author's account key (`PostMediaPresignSignBytes`) over
  `"pp-browser:post-media-v1" NUL ‖ u8 1 ‖ LenUtf8 account_id ‖ 32 B content_id ‖ u64 BE size ‖ LenUtf8 mime ‖
  i64 BE issued_at` (fresh within 5 minutes), because the bytes are not signed by an entry until the post
  is published.
- **Cross-implementation vectors:** `src/lib/posts/tests/data/post_vectors.json` — entries of every kind,
  a head and a media presign, signed by `lib/posts`; every implementation (the relay) must parse, hash and
  verify them alike.
- The relay enforces the protocol limits (S014) and **its quotas per author**, stated in its `post_relay`
  terms. A relay may also require registration on its own terms; the default relay serves registered accounts.
- **Forks** are stored side by side (both entries are proof) and flagged on the author (S019).
- **Deletes:** a `delete` entry stubs its target — bytes dropped, place and hash kept — and unpins the
  target's media once no kept entry names them.
- Media objects are keyed by `content_id`; the relay never trusts the declared hash (readers check every
  byte against it), and counts the declared size against the quota.

### Reads — anonymous (S011)

| Request | Answer |
|---------|--------|
| `GET /posts/heads/{account_id}` | `{ "head": b64, "key_card": {…} }` — the newest head held, and the key card that verifies it |
| `GET /posts/entries/{account_id}/{lane_hex}?from={seq}&limit={n ≤ 256}` | `{ "entries": [b64 \| { "stub": "deleted", "seq", "entry_hash" }], "next_from"? }` in seq order; forks both included |
| `GET /posts/media/{content_id_hex}` | the bytes, or a redirect to the relay's CDN |

No read is signed. A relay sees the reader's network address; reaching the relay through a mesh circuit
hides it (later).

### Storage and terms

Storage is the relay's choice (the default relay: its database for entries and heads, its blob store for
media). It keeps stubs and forks. Retention is stated in its terms (default: entries kept; media one year).
Indexing and counts (S007) are later additions to this API.

---

## 3. Device fetch over Amp

App protocol **`/pp/posts/1.0.0`**, one JSON request and reply per stream, the same shapes as the reads:

```json
→ { "op": "get_head", "account_id": "account:…" }
← { "head": "…", "key_card": { … } }   |   { "error": "not_available" }
→ { "op": "get_entries", "account_id": "account:…", "lane": "hex", "from": 1, "limit": 256 }
← { "entries": [ … ], "next_from": 257 }
```

Media comes by the public CAS fetch (content id over the blob path).

- **v1 serves its own account only** (S020): a device answers for the account it belongs to. Serving
  authors it merely follows (peer mirroring) comes later.
- Requests are unauthenticated, but answering is the author's device: **fetching from the author's devices
  tells the author who reads.** Readers prefer the author's relays and use devices as the fallback (§ 4).

---

## 4. Head discovery

For a followed author, a reader gathers heads and keeps what they prove:

1. **Sources, in order:** the targets of the newest head it holds → hints (locator, contact card) → its
   own relays (the default relay) → the author's devices (account card devices, over Amp).
2. **Verify** each head (the key from the reply's `key_card`, the card store, or the directory — every
   source checked against the account id).
3. **Targets** come from the newest valid head (`issued_at`); **lanes** are the per-lane maximum seq across
   all valid heads seen — a stale or partial head cannot hide a lane another head names.
4. Stop asking once a source returns a head at least as new as the one held.

## 5. Publishing (author)

```text
compose → media into the public CAS realm (content ids) → PostAuthor.Append(entry) → IssueHead(targets)
        → publish outbox, per target:  media presign + upload (skipped when "present")
                                        → POST entries (oldest unpublished first) → POST head
```

- The outbox retries with backoff and survives restarts; per target it remembers the highest seq
  published per lane.
- The post shows where it is: *published to 2 of 2*, *waiting for relay R*.
- The author's other devices learn the new lane head from the targets (or over Amp) and fold it into
  their next head statement.

## 6. Following and refreshing (reader)

- **Follow list:** local (`post_follows`: account, added_at, last_refreshed). No public list (S012).
- **Refresh** (opening the feed, and every 15 minutes while it is visible): head discovery (§ 4), then per
  lane fetch from `held seq + 1` in pages of ≤ 256 from the targets (devices as fallback), verify each
  entry, check the chain, keep it (`PostStore`). Media is fetched when a post is shown, checked against its
  content id.
- **Kept:** forks side by side (the author is marked); unknown kinds and over-limit entries as stubs; gaps
  filled as later pages arrive.
- **Keys:** a key card that verified a head is kept in the card store (source `post_relay`), so the author's
  keys are known for later reads and for chat.
- **Feed order:** by claimed time, badged *claimed* (P2); witnessed and anchored badges come in P4–P5.

## 7. Later

- **Subscribe** (S012): register with a target (signed) for pushes — the relay's existing wake path or the
  [announce plane](../peer-scoped-broadcast/DESIGN.md) scoped to the account.
- **Peer mirroring:** devices serving authors they follow (§ 3).
- **Indexes and counts** on relays (S007, S009), with replies and reactions (P3).

## What P2 builds

| Side | Work |
|------|------|
| Relay (app-static-api PR, merged and deployed first) | the API of § 2: verify entries and heads (ML-DSA, limits, quotas), store with forks and stubs, media presign and serve |
| Client | head targets; the publish outbox (§ 5); `/pp/posts/1.0.0` (§ 3); head discovery and refresh (§ 4, § 6); follow list; compose with text and images; a minimal feed showing *claimed* time |
| Lab | a scenario: Alice posts with an image; Bob follows her through her invite, refreshes, verifies and sees it; a delete reaches him as a stub |
