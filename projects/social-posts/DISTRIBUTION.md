# Social posts — distribution

**Status:** Draft (2026-10-23; carried copies, moderation and notices 2026-10-30), for P2  
**Builds on:** [DESIGN.md](DESIGN.md) (log, heads, trust ladder) · decisions S017–S025, S029 in [DECISIONS.md](DECISIONS.md)

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
| `POST /posts/entries` | `{ "entries": [signed entry b64, …≤ 32], "key_card"?: {…}, "carried"?: [{ "entry": b64, "key_card"?: {…} }, …≤ 32] }` | per entry `{ "entry_hash", "status": "stored" \| "duplicate" \| "fork" \| "refused", "reason"? }`; the same per carried entry under `"carried"` |
| `POST /posts/heads` | `{ "head": signed head b64, "key_card"?: {…} }` | `{ "status": "stored" \| "older" \| "refused" }` |
| `POST /posts/media/presign` | `{ "account_id", "content_id", "size", "mime", "issued_at", "signature" }` | `{ "upload_url", "public_url" }`, or `{ "present": true }` when held, or `{ "error": "withheld" }` |
| `POST /posts/reports` | `{ "subject": { "entry": ref } \| { "content_id" }, "reason", "note"?, "reporter"?: { "account_id", "issued_at", "signature", "key_card"? } }` | `{ "status": "received" }`, or `{ "error": "rate_limited" }` |

- An entry or head **is** its own authentication: the relay verifies the signature against the author's
  key (from its directory, or the `key_card` sent along — checked against the account id like any card).
- **Whose entries a relay keeps** (S029): those of the authors it serves, and the copies they carry. Under
  `"carried"` it accepts another author's entry when a `repost` or a quote by an author it serves names it
  (in the same request or already held), and that author's `delete` of an entry it holds as a carried copy
  (a forwarded delete, which stubs the copy). Anything else from an author it does not serve is refused as
  `not_served`. Carried copies count against the carrying author's quota.
- **Refusal reasons** are codes: `malformed`, `signature`, `unknown_author_key` (send the key card),
  `limits`, `quota`, `not_served`, `not_carried` (no held repost or quote names it), `suspended`,
  `withheld`; a human-readable `message` may come alongside.
- The media presign is signed by the author's account key (`PostMediaPresignSignBytes`) over
  `"pp-browser:post-media-v1" NUL ‖ u8 1 ‖ LenUtf8 account_id ‖ 32 B content_id ‖ u64 BE size ‖ LenUtf8 mime ‖
  i64 BE issued_at` (fresh within 5 minutes), because the bytes are not signed by an entry until the post
  is published.
- A signed **report** (S025) is signed by the reporter's account key over
  `"pp-browser:post-report-v1" NUL ‖ u8 1 ‖ LenUtf8 reporter ‖ u8 subject kind (1 entry, 2 media) ‖ subject
  (entry: LenUtf8 author ‖ 16 B lane ‖ u64 BE seq ‖ 32 B entry_hash; media: 32 B content_id) ‖ LenUtf8 reason ‖
  LenUtf8 note ‖ i64 BE issued_at` (fresh within 5 minutes). Reasons: `illegal`, `sexual`, `violence`,
  `harassment`, `spam`, `other`; the note is ≤ 1 KiB. An anonymous report omits `reporter`. The relay
  rate-limits reports by network address and by reporter, and may weigh signed ones more.
- **Cross-implementation vectors:** `src/lib/posts/tests/data/post_vectors.json` — entries of every kind,
  a head, a media presign and a report, signed by `lib/posts`; every implementation (the relay) must parse,
  hash and verify them alike.
- The relay enforces the protocol limits (S014) and **its quotas per author**, stated in its `post_relay`
  terms. A relay may also require registration on its own terms; the default relay serves registered accounts.
- **Forks** are stored side by side (both entries are proof) and flagged on the author (S019).
- **Deletes:** a `delete` entry stubs its target — bytes dropped, place and hash kept — and unpins the
  target's media once no kept entry names them.
- Media objects are keyed by `content_id`; the relay never trusts the declared hash (readers check every
  byte against it), and counts the declared size against the quota.
- **Scan before serving** (S024): an uploaded object is served only after the relay's scan step has passed
  it (a step that may pass everything at first); until then it is `processing`.
- **Moderation** (S023, [DESIGN § 7](DESIGN.md#7-moderation)): the relay may withhold an entry (by hash,
  covering carried copies), a media object (by content id) or an author (`suspended`). Withheld entries are
  served as stubs; withheld content sent again, or as a carried copy, is refused as `withheld`.

### Reads — anonymous (S011)

| Request | Answer |
|---------|--------|
| `GET /posts/heads/{account_id}` | `{ "head": b64, "key_card": {…} }` — the newest head held, and the key card that verifies it |
| `GET /posts/entries/{account_id}/{lane_hex}?from={seq}&limit={n ≤ 256}` | `{ "entries": [b64 \| stub], "carried"?: [{ "entry": b64 \| stub, "key_card" }], "next_from"? }` in seq order; forks both included; `carried` holds the copies the page's reposts and quotes name |
| `GET /posts/entry/{account_id}/{lane_hex}/{seq}` | `{ "entries": [b64 \| stub], "key_card" }` — one place, held or carried (both entries of a fork) |
| `GET /posts/media/{content_id_hex}` | the bytes, or a redirect to the relay's CDN; `{ "error": "processing" }` (HTTP 409) before the scan passes it; `{ "error": "withheld", "reason" }` (HTTP 451) |

A **stub** is `{ "stub": "deleted" | "withheld", "reason"?: "legal" | "terms", "seq", "entry_hash" }`: the
place and hash without the bytes, so the chain still verifies (S016, S023).

No read is signed. A relay sees the reader's network address; reaching the relay through a mesh circuit
hides it (later).

### Storage and terms

Storage is the relay's choice (the default relay: its database for entries and heads, its blob store for
media). It keeps stubs and forks. Retention is stated in its terms (default: entries kept; media one year).
Indexing and counts (S007) are later additions to this API. Its `post_relay` terms state its moderation
policy (S023) and, later, whether it accepts notices (S029).

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
- **Reposts and quotes** (S022, P3): the outbox also uploads the named entry's media (under the reposter's own
  presign) and sends the entry with its author's key card under `"carried"`. The client keeps watching the
  authors it carried for deletes, and forwards each one under `"carried"` to every target it sent the copy.
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
- **Notices** (S029, P3): the client indexes, from the logs it fetches, the entries that name the user
  (`reply_to` / `reply_root`, `quote`, `repost` / `react` targets, mention facets). Nothing is sent to anyone
  else's relays, so notices come only from the people the user follows.
- **Threads** (P3): a thread view shows replies from the reader's follows; *more replies* is a tap, which asks
  the root author's targets and the reader's own relays (the `replies` read of § 7). Nothing is fetched for
  strangers automatically.

## 7. Later

- **Subscribe** (S012): register with a target (signed) for pushes — the relay's existing wake path or the
  [announce plane](../peer-scoped-broadcast/DESIGN.md) scoped to the account.
- **Peer mirroring:** devices serving authors they follow (§ 3).
- **Indexes and counts** on relays (S007, S009), with replies and reactions (P3): first
  `GET /posts/replies/{account_id}/{lane_hex}/{seq}` — the entries the relay holds whose `reply_root` names
  that entry (its own authors' only, S029).
- **Reserved** (S029): accepting entries that name the authors a relay serves, a `refs` read (entries naming
  an account), an `accepts_notices` term, a price for stranger notices.

## What P2 builds

| Side | Work |
|------|------|
| Relay ([app-posts](https://github.com/nfsc-brief-web3/app-posts), behind the app-static-api route; merged and deployed first) | the API of § 2: verify entries and heads (ML-DSA, limits, quotas), store with forks and stubs, media presign and serve; only served authors (`not_served`), refusal codes, withheld stubs, the scan step before media is served, reports |
| Client | head targets; the publish outbox (§ 5); `/pp/posts/1.0.0` (§ 3); head discovery and refresh (§ 4, § 6); follow list; compose with text and images; a minimal feed showing *claimed* time |
| Lab | a scenario: Alice posts with an image; Bob follows her through her invite, refreshes, verifies and sees it; a delete reaches him as a stub |
