# Social posts — design

**Status:** Draft (2026-10-23)  
**Decisions:** [DECISIONS.md](DECISIONS.md) · **Phases:** [PHASES.md](PHASES.md)

## Problem

Web2 social media rests on one operator: it stores every post, stamps the time, counts the likes and
decides who sees what. pp-browser has no such operator. Posts live on the author's devices; relays are
services the author picks; anyone can claim any time and rewrite their own history. We want public posts
that still mean something:

- a reader can tell a post is really the author's, wherever it came from;
- a reader can tell the author has not quietly rewritten what they said;
- a time can be trusted when it matters — not because the author says so;
- social actions (reply, like, repost) work without a central counter, and say honestly what they know.

Scope (S001): **public posts only.** No followers-only circles, no private feeds.

## Model in one picture

```text
 Account (S002) ──signs──► Lane A (device 1): e1 ─► e2 ─► e3 ─► …     each entry: seq, prev hash,
                └─signs──► Lane B (device 2): e1 ─► e2 ─► …            claimed time, kind, body
                                      │
                         Head statement (signed): {lane A: seq 3 / hash, lane B: seq 2 / hash}
                                      │
         trust ladder:  claimed ──► witnessed (service receipts) ──► anchored (pp-ledger)
```

Media bytes are not in the log: entries point at objects in the public CAS realm
([content-cas](../content-cas/DESIGN.md)).

---

## 1. The post log

### Author and lanes

The author is the **account** (S002), proven by the account's ML-DSA-65 key as in
[ACCOUNT_CARD.md](../../docs/contracts/ACCOUNT_CARD.md). Every linked device holds that key, and two
devices posting at once must not fork one chain — so each device writes its **own lane**: a separate
hash chain under the same account. A reader's view of an author is the union of their lanes.

- A lane id is **16 random bytes** chosen by the device when it first posts (S003) — not its PeerId,
  which the account card discloses only to authenticated, unblocked requesters
  ([card exchange](../../docs/contracts/ACCOUNT_CARD.md#card-exchange-over-amp)). Public posts must not
  publish the device list.
- The lane id is device state, never in a backup: a restored device starts a **new lane**, so it cannot
  reuse sequence numbers it no longer remembers. The writer (`PostAuthor`) also binds its lane to the
  device's PeerId, and starts a new lane whenever the store holds entries in its lane past what it wrote —
  so even a device whose own record went back never signs a second entry at a seq.
- Lanes are cheap and permanent. An unused lane simply stops.

### Entry

An entry is its **canonical bytes** plus the account's signature over them. Encoding follows the PP
binary wire profile ([WIRE_SCHEMAS § D088](../../docs/contracts/WIRE_SCHEMAS.md#pp-binary-wire-profile-d088)):

```text
domain   "pp-browser:post-entry-v1" NUL
u8       version = 1
LenUtf8  author        account id ("account:…")
16 bytes lane
u64 BE   seq           1, 2, 3, … per lane
32 bytes prev          entry hash of seq − 1 in this lane; zeros when seq = 1
i64 BE   claimed_at    ms since epoch, the author's clock
u8       kind
LenBytes body          kind-specific canonical encoding (below)
```

- **`entry_hash` = BLAKE2b-256(canonical bytes).** The signature is *not* hashed (S004): ML-DSA
  signatures are randomized, so the same entry signed twice must keep one identity.
- **`signature`** = ML-DSA-65 with the account key over the canonical bytes.
- Stored and served as `canonical bytes ‖ signature`. A JSON rendering exists for tools only; it is
  never signed or hashed.

### Kinds (v1)

| kind | name | body |
|------|------|------|
| 1 | `post` | text (LenUtf8); media: count + `{content_id 32 B, mime LenUtf8, size u64}`; optional `reply_to` ref; optional `quote` ref |
| 2 | `edit` | target ref (own entry, kind `post`); replacement `post` body |
| 3 | `delete` | target ref (any own entry) |
| 4 | `react` | target ref; reaction LenUtf8 (v1 UI: `like`) |
| 5 | `repost` | target ref |
| 6 | `profile` | display name, bio, avatar `content_id`, services offered — the newest wins (§ 5) |
| 7 | `lane_close` | lane, last seq: this lane ends there (a retired or lost device) |
| 8 | `disavow` | target refs (any author lane): "these are not mine" (§ 1, Lost devices) |

A **ref** names one exact entry: `{author, lane, seq, entry_hash}`. The hash pins content: a ref to an
entry that was later edited still points at the version it meant.

Unknown kinds are kept, chained and skipped by the UI, so a newer client's log stays valid to an older one.
Undoing a like or a repost is a `delete` of one's own `react` / `repost` entry — no separate "unlike" kind.

### Validity

An entry is valid on its own when:

1. the signature verifies with the author's account key (any key source in
   [ACCOUNT_CARD § Resolving keys](../../docs/contracts/ACCOUNT_CARD.md#resolving-keys));
2. `seq ≥ 1`, and `prev` is zeros exactly when `seq = 1`;
3. `claimed_at` is no more than 5 minutes ahead of the reader's clock;
4. the body decodes for its kind and is within the protocol limits (§ 6); refs are well formed;
   `edit` / `delete` / `disavow` targets name the same author.

With its predecessor at hand it is also **chained**: `prev` = the predecessor's `entry_hash`, and
`claimed_at ≥` the predecessor's `claimed_at`. A reader may hold a lane with gaps (it fetched e7 before
e3); each entry is still valid, and continuity is checked as gaps fill.

### Forks

Two valid entries with the same `(author, lane, seq)` and different `entry_hash` are a **fork**: the
author signed two histories. The pair is self-contained proof — anyone can check it — and readers keep
both, mark the lane forked, and show it on the author. Honest clients cannot fork (one device per lane,
new lane after restore), so a fork means a bug or a lie.

### Head statement

What an author's log looks like now, signed so that a relay cannot hide a lane:

```text
domain   "pp-browser:post-head-v1" NUL
u8       version = 1
LenUtf8  author
i64 BE   issued_at
u64 BE   lane count, then per lane (sorted by lane id): 16 B lane, u64 BE seq, 32 B entry_hash
u64 BE   target count (≤ 8), then per target: LenUtf8 url — the author's publish targets (S017)
```

`head_hash` = BLAKE2b-256(canonical bytes); signed like an entry. A device issues a new head after it
posts, folding in the latest heads it knows of its sibling lanes. Readers and relays keep the newest
`issued_at` per author and take the per-lane maximum `seq` across statements. Heads are what discovery
answers and what anchors commit to (§ 2).

### Lost devices

Whoever holds a lost device holds the account key, so nothing signed by that key can settle who is
honest. What the author can do (S015):

- **`lane_close`** — "lane L ends at seq N", from a device still held. Entries past N in that lane are
  shown as *after the lane was closed*. Retiring a device normally closes its lane the same way.
- **`disavow`** — "these entries are not mine", for posts already out.

The other side can sign the same kinds; readers show conflicting claims side by side, and an anchored
claim outranks a later one. The real fix is rotating the account key
([M019](../multi-device-account/DECISIONS.md), later).

### Edit and delete

- **Edit:** a new entry; the original stays in the log (public history). Readers show the newest edit
  with an *edited* mark and can open earlier versions.
- **Delete** (S008): a tombstone. Honest clients and relays stop showing the target and its edits and
  unpin its media. They may keep a **stub** — the target's `entry_hash`, lane and seq without its bytes —
  so the chain past it still verifies. Copies elsewhere, and any anchor, outlive the delete; the UI says
  so before the user deletes.

---

## 2. Time: the trust ladder

A post's time is only as good as who vouches for it.

| Level | Source | Means | Badge |
|-------|--------|-------|-------|
| **Claimed** | the entry's `claimed_at` | what the author says | *claimed* |
| **Witnessed** | receipts from services that saw the entry or a head | "existed by T, says W" — as good as W | *seen by W at T* |
| **Anchored** | a head committed in a pp-ledger block | "existed no later than block time" — trusted | *anchored* |

### Witness receipts

A witness is an ordinary account that offers the *witness* service in its profile (S013) — usually a
publish target. It countersigns what it received:

```text
domain   "pp-browser:post-witness-v1" NUL
u8       version = 1
LenUtf8  witness account id
32 bytes subject      entry_hash or head_hash
u8       subject kind (1 entry, 2 head)
i64 BE   seen_at
```

signed by the witness account's key. Publish targets return one on publish (P4). Several independent
receipts narrow the window; none of them is proof. Which witnesses count is the reader's choice, like
whom to follow: by default, receipts from contacts and followed accounts are shown first.

### Anchors

An **anchoring service** (an account offering it, S013) batches heads from many authors and commits one
Merkle root per period (e.g. hourly) in one pp-ledger transaction, so anchoring costs an author almost
nothing; an author who cares about one post may anchor it directly. What is committed:

- leaves are `head_hash`es; one Merkle root per anchor transaction, so one transaction covers many
  authors and, through the chains, every entry up to each anchored head;
- an **anchor proof** for a post = the entry, the chain from it to an anchored head (or the head itself
  when it names that entry's seq), the head statement, the Merkle path, and the transaction's place in
  the ledger;
- verifying it needs the ledger (through a `ledger_gateway`, [N029 phase C](../p2p-mesh/NAME_DIRECTORY_NORTH_STAR.md)).

An anchor proves **no later than** (S006). That is the property that matters: once a head is anchored,
nothing at or below its sequence numbers can be backdated or rewritten without a provable fork. It never
proves "no earlier" — an author can sit on a post — and the UI never implies it.

**Inconsistency:** an entry at or below an anchored seq whose `claimed_at` is *after* the anchor's block
time claims an impossible time; readers flag it. Anchoring is opt-in and batched — not every post
needs a transaction.

The pp-ledger side (the anchor transaction type, fees, who may batch) is a pp-ledger design; this side
fixes only what is committed (`head_hash` leaves) and what a proof contains.

---

## 3. Distribution (detail: [DISTRIBUTION.md](DISTRIBUTION.md))

Storage is local first: the author's lanes and the logs a user follows are kept in profile.db — public
material, plaintext like peer cards: `post_entries` (each entry's place, hash, kind, claimed time and
signed bytes; forks side by side; stubs without bytes), `post_heads` (each author's newest head) and
`post_own_lanes` (this device's lane); media in the public CAS realm. Relays serve posts free; posts are small (§ 6).

| Path | Carries | Notes |
|------|---------|-------|
| **Publish targets** | entries, heads, media | relays the author picks (S007); the default relay is one of them. Listed in the head statement (S017), hinted in invites. |
| **Author's devices** | entries, heads | over Amp when reachable; same verification |
| **Announce plane** | "new head" tips | [peer-scoped-broadcast](../peer-scoped-broadcast/DESIGN.md), scoped to the **account** rather than a PeerId |
| **Peers who have it** | entries, media | any holder can serve: everything self-verifies |

**Follow** = a local list: keep each followed author's newest head and fetch past what is held. Two ways
to read, the reader's choice per author (S012):

| Mode | How | Who learns you follow |
|------|-----|-----------------------|
| **Fetch** | poll heads and entries | no one by name: **reading public posts never requires identifying yourself** (S011) — reads are unsigned; a relay still sees the network address (fetch through a circuit to hide it, later) |
| **Subscribe** | register (signed) with a publish target or the author's devices for pushes | whoever you subscribe through |

There is no public follow list: what a relay sees of subscriptions is all anyone can know, and fetching
leaves even less. Head discovery: ask the author's publish targets, the author's devices, and (later)
peers who follow them; keep the newest signed head.

Relays may index and aggregate (S007): search, counts, "who replied". Everything they say about counts
is attributed to them (§ 4).

---

## 4. Social actions (sketch — detailed before P3)

Who owns the data decides where it lives and what can be known.

| Action | Lives in | Seen by the author | Counted |
|--------|----------|--------------------|---------|
| post, edit, delete, profile | the author's log | — | — |
| reply, quote | the **replier's** log (`post` with `reply_to` / `quote`) | a notice to the author (optional DM-like ping) | as seen by X |
| like (`react`), repost | the **reactor's** log, public (S005) | optional notice | as seen by X |

- **Counts are local views** (S009): "12 likes from people you follow", "≥ 340 seen by relay R" — never
  a bare global number.
- **Replies under a post** default to the author's accepted replies and replies from people the reader
  follows; the rest are a tap away. The follow graph is the spam filter; no one speaks *on* someone
  else's log (the [publisher-sole-author](../peer-scoped-broadcast/DESIGN.md#speak--reply-rules) rule).

---

## 5. Profiles and services

A `profile` entry (display name, bio, avatar) in the author's log: signed, chained and anchorable like
any post, newest wins. The account card's nickname stays the card's; the profile entry is what posts show.

Service providers are ordinary accounts (S013). A profile may list **services offered**, each a kind
with its terms:

| Service | Terms it states |
|---------|-----------------|
| `post_relay` | quotas (§ 6), retention, whether it indexes / counts |
| `witness` | — |
| `anchor` | batch period, ledger |

The contact panel shows "Provides: …" for any account, and Settings offers known providers when the
user picks publish targets.

## 6. Limits

Two layers (S014): **protocol limits** every client enforces — so a reader knows the most a post can
cost — and **relay quotas**, which a relay states in its `post_relay` service and may set lower.

| Protocol limit | Value |
|----------------|-------|
| Entry canonical bytes | ≤ 8 KiB |
| Post text | ≤ 4 KiB UTF-8 |
| Media per post | ≤ 4 |
| Media object | ≤ 16 MiB |
| Lanes in a head statement | ≤ 64 |
| Entries per fetch page | ≤ 256 |

| Relay quota (example default) | Value |
|-------------------------------|-------|
| Entries per account per day | 100 |
| Media per account per day | 256 MiB |
| Retention | stated by the relay (e.g. entries forever, media 1 year) |

Over a relay quota, that relay refuses an entry and others may not.

### Adjusting limits (S016)

**A reader that will not keep an entry does not break the chain.** An entry over the reader's limits, or
of a kind it does not know, is kept as a **stub** (lane, seq, `entry_hash`, size — the same stub as after a
delete) and the lane continues past it. So raising a limit later only means older clients skip the larger
entries; it does not cut the author's lane for them.

| Limit | Raise | Lower | Why |
|-------|-------|-------|-----|
| Relay quotas | easy | easy | each relay's policy, stated in its terms |
| Entries per fetch page | easy | easy | a request parameter; a relay may return fewer |
| Lanes per head | easy | easy | heads are reissued, not history (anchored heads keep the rule they were made under) |
| Media size / count | easy | hard | media is fetched apart and can be declined; lowering would hide existing posts |
| Entry / text size | moderate (with stubs) | not for an existing version | inside signed, hashed bytes; lowering would invalidate published and anchored history |

Never lower a protocol limit for an existing entry version; start conservative on what is hashed. Not
limits but fixed for a version: the canonical encoding, hash, signature scheme and field widths — changing
them is a new entry version (the version byte).

---

## Open questions

1. **Anchor transaction** — its type, fee and batching rules on the pp-ledger side.
2. **Video** — 16 MiB per object fits photos and short clips; longer video likely needs chunked CAS
   objects (a later media kind), not a bigger limit.
