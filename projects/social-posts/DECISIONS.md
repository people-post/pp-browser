# Social posts — decisions

Design: [DESIGN.md](DESIGN.md).

---

## S001 — Public posts only

**Status:** Accepted (2026-10-23, product)

**Decision:** Every post is public. No followers-only circles or private feeds in this project.

**Rationale:** Circles mean encrypting to a changing audience; public-only lets the log, distribution
and trust ladder be designed without it. Private sharing stays in chat.

---

## S002 — The author is the account

**Status:** Accepted (2026-10-23, product)

**Decision:** Posts are signed by and attributed to the Account ID, not a device PeerId.

**Rationale:** A person posts from any of their devices; account cards already prove keys and devices
with no relay. The [peer-scoped-broadcast](../peer-scoped-broadcast/) announce plane follows (account
scope, not PeerId).

---

## S003 — One lane per device, random lane id

**Status:** Accepted (2026-10-23)

**Decision:** Each device writes its own hash chain (lane) under the account. A lane id is 16 random
bytes, device state never restored from backup.

**Rationale:** Linked devices share the account key; one chain would fork whenever two devices post at
once. PeerIds as lane ids would publish the device list, which the account card discloses only to
authenticated requesters. A restored device cannot know which seqs it used, so it starts a new lane.

---

## S004 — Entry hash excludes the signature

**Status:** Proposed (2026-10-23)

**Decision:** `entry_hash` = BLAKE2b-256 of the canonical bytes alone; the signature is carried beside them.

**Rationale:** ML-DSA signatures are randomized: hashing them would give one entry many identities.
Signing the same bytes again (a re-send, a second device's backfill) must not look like a fork.

---

## S005 — Reactions are public entries in the reactor's log

**Status:** Accepted (2026-10-23, product)

**Decision:** Likes and reposts are `react` / `repost` entries in the reacting account's own log, public
like any post. Undo is a `delete` of that entry.

**Rationale:** No central counter exists; the only place a like can live is with whoever liked.

---

## S006 — Anchors prove "no later than", nothing more

**Status:** Proposed (2026-10-23)

**Decision:** pp-ledger anchors commit head hashes (Merkle-batched); a post's trusted time is "existed no
later than the block". The UI never presents an anchor as "posted at".

**Rationale:** A chain can bound when something existed, not when it was written; an author can always
hold a post back. "No later than" is enough to stop backdating and silent rewrites below an anchored head.

---

## S007 — Relays are chosen publish targets and may index

**Status:** Accepted (2026-10-23, product)

**Decision:** The default relay is one publish target among any the author lists. Relays may index and
aggregate (search, counts) as an optional service.

**Rationale:** Indexing is useful and optional; it is safe because everything a relay serves self-verifies
and everything it counts is attributed to it (S009).

---

## S008 — Delete is a tombstone

**Status:** Accepted (2026-10-23, product)

**Decision:** Delete hides the target for honest clients and relays (which may keep a hash-only stub so
the chain still verifies). The UI states that copies and anchors may remain.

**Rationale:** A public, self-verifying post cannot be recalled from everyone who has it; promising
otherwise would be false.

---

## S009 — Counts are always "as seen by"

**Status:** Proposed (2026-10-23)

**Decision:** Every count names its scope: people you follow, a named relay, this device. No unattributed
global totals.

**Rationale:** No one sees every reaction. A number without its source would be a claim nobody can back.

---

## S010 — Times are badged by trust level

**Status:** Proposed (2026-10-23)

**Decision:** A post's time is shown as *claimed*, *seen by W at T*, or *anchored*; an entry whose claimed
time contradicts an anchor is flagged.

**Rationale:** The author's clock is a claim; the reader deserves to know what vouches for a time.

---

## S011 — Reading public posts is anonymous

**Status:** Accepted (2026-10-23)

**Decision:** Fetching heads and entries is unsigned; no relay or author may require a reader to identify
themselves to read public posts.

**Rationale:** Public means public. A relay still sees the network address; fetching through a circuit
can hide that later.

---

## S012 — Follows are local; subscribing is a choice

**Status:** Accepted (2026-10-23, product)

**Decision:** No public follow list and no `follow` kind. A reader per author either *fetches* (polls, no
identity) or *subscribes* (registers for pushes, and whoever they subscribe through learns it).

**Rationale:** A follow graph is only as public as the reader makes it: what a relay sees of subscriptions
is all anyone can know.

---

## S013 — Service providers are ordinary accounts

**Status:** Accepted (2026-10-23, product)

**Decision:** Relays, witnesses and anchoring services identify as accounts like anyone, and state the
services they offer (with terms) in their `profile` entry. Which witnesses count is the reader's choice.

**Rationale:** No second identity system for services; trust in a service is decided the way trust in a
person is. The contact panel can show what any account provides.

---

## S014 — Protocol limits and relay quotas

**Status:** Accepted (2026-10-23, product)

**Decision:** Hard protocol limits every client enforces (entry ≤ 8 KiB, text ≤ 4 KiB, ≤ 4 media of
≤ 16 MiB each, ≤ 64 lanes per head, ≤ 256 entries per page), and per-relay quotas stated in its service
terms, never above the limits. Relays serve posts free.

**Rationale:** Relays need predictable load to offer a free service; readers need to know the most a post
can cost before fetching it.

---

## S015 — Lost devices: close the lane, disavow, rotate later

**Status:** Accepted (2026-10-23, product)

**Decision:** `lane_close` ends a lane at a seq (also used to retire a device); `disavow` disowns
entries already out. Conflicting claims are shown side by side; an anchored claim outranks a later one.

**Rationale:** Anyone holding the account key can sign anything, so these are statements, not proofs;
the full fix is account-key rotation (M019).

---

## S016 — Stubs keep chains whole; protocol limits only grow

**Status:** Accepted (2026-10-23)

**Decision:** An entry a reader will not keep (over its limits, an unknown kind, deleted) is kept as a stub
— lane, seq, `entry_hash`, size — and the lane continues. Protocol limits for an entry version may be
raised, never lowered.

**Rationale:** Limits inside signed, hashed bytes outlive any client. With stubs, raising one later costs
older clients only the larger entries; lowering would invalidate history already published and anchored.

---

## S017 — Publish targets live in the head statement

**Status:** Proposed (2026-10-23)

**Decision:** The author's publish targets (≤ 8 https URLs) are part of the signed head statement; invites
and contact cards carry them only as unsigned hints.

**Rationale:** The head already travels with the log and is newest-wins; putting targets in the account
card would need a card version older clients reject.

---

## S018 — Writes are authenticated by the author's own signatures

**Status:** Proposed (2026-10-23); amended by S029 (2026-10-30) — a relay keeps only the authors it serves and the copies they carry

**Decision:** A relay accepts an entry or head because it verifies against the author's key (sent along as a
key card when the relay does not know it); media uploads carry an account-signed presign request. Reads are
unsigned (S011). A relay may additionally require registration on its own terms.

**Rationale:** The signed objects are their own proof of authorship, so anyone may mirror them; quotas key
on the author either way.

---

## S019 — Relays keep forks and stubs, and apply deletes

**Status:** Proposed (2026-10-23); extended by S023 (2026-10-30) — withheld stubs

**Decision:** Relays store both entries of a fork and flag the author; a `delete` entry turns its target
into a stub (place and hash kept) and unpins media no kept entry names.

**Rationale:** A fork is proof readers should be able to fetch; a stub keeps the chain verifiable after a
delete (S008, S016).

---

## S020 — Devices serve their own account's posts, nothing else (v1)

**Status:** Proposed (2026-10-23)

**Decision:** `/pp/posts/1.0.0` answers for the device's own account only. Readers try the author's relays
before the author's devices.

**Rationale:** Answering from the author's device reveals the reader to the author; relays are the private
default, devices the fallback. Peer mirroring of followed authors comes later.

---

## S021 — P2 is fetch only

**Status:** Proposed (2026-10-23)

**Decision:** The first slice polls (on opening the feed and every 15 minutes while visible). Subscriptions
with pushes come after.

**Rationale:** Fetch needs no registration and reveals nothing by name (S011, S012); pushes add a relay-side
subscription model worth designing on its own.

---

## S022 — Reposts and quotes carry a copy beside them

**Status:** Accepted (2026-10-30, product)

**Decision:** A repost or quote names the exact version it means (the ref's `entry_hash`), and the reposter
publishes that entry — with its author's key card and media — to the reposter's own targets beside the
repost, not inside it. Readers get the post from the reposter's relay alone and verify it as the author's.
The copy is for speed and availability, not permanence: honest clients and relays honor the author's
`delete` when they learn of it, and the reposter's client forwards it to where it sent the copy. Edits do not
follow a copy (it shows the reposted version, marked *edited later* when a newer one is known). Replies do
not carry their parent; a repost of a repost names and carries the original.

**Rationale:** The ref already pins a snapshot; what readers lacked was a source. Embedding would not fit the
entry limit (an entry plus a 3.3 KB signature inside an 8 KiB entry) and would put the author's post inside
the reposter's signed bytes, where neither a delete nor a takedown could drop it without breaking the
reposter's chain. Carried copies are stored once per relay, keyed by hash, and readers never contact the
original author's relays, so those relays do not learn who reads the reposter.

---

## S023 — A relay moderates what it holds; withheld is not deleted

**Status:** Accepted (2026-10-30, product)

**Decision:** A relay meets its legal obligations and its own terms for everything it holds. It may withhold
an entry (by hash, which covers carried copies of it), a media object (by content id, in every post that names
it) or an author (refused writes, `suspended`, and what it holds withheld). A withheld entry is served as a
stub with reason `legal` or `terms`, so the chain still verifies; it binds that relay only, and readers fall
back to the author's other targets as usual. A carried copy of withheld content is refused. Mute and block
lists are the reader's, local and never published.

**Rationale:** Posts on a relay are public plaintext: the relay sees all of it and answers for it. An author's
delete and a relay's withholding mean different things to a reader, and one relay's obligations must not
read as the author's act or bind other relays.

---

## S024 — Media is served after the relay's scan step

**Status:** Accepted (2026-10-30, product)

**Decision:** An uploaded media object is not served until the relay's scan step has passed it; until then
reads answer `processing`. The step exists from the first relay release, even when it does nothing yet.

**Rationale:** Uploads go straight to the blob store by presigned URL, so the relay never sees the bytes on
the way in. Checking before serving is the only point where known-bad media can be stopped; reserving the
step keeps adding a real scanner a relay-side change with no client change.

---

## S025 — Reports may be anonymous or signed, and are rate-limited

**Status:** Accepted (2026-10-30, product)

**Decision:** A relay takes reports on an entry or a media object. A report may be anonymous or signed by the
reporter's account; signed reports may weigh more. Both are rate-limited.

**Rationale:** Reading is anonymous (S011), so requiring an account to report would shut out the readers who
see the content; signatures let a relay tell accountable reports from noise.

---

## S026 — The post body carries what readers need before media and for filters

**Status:** Accepted (2026-10-30, product)

**Decision:** `post` v1 (defined before anything was published) carries, per media item, alt text,
width/height, duration and an optional placeholder; per post, facets (byte spans of the text marked as a
mention, a link or a hashtag), a content warning and self-labels, up to three languages, and `reply_root`
beside `reply_to`.

**Rationale:** Signed bodies cannot gain fields later without a new kind or version, and nothing was published
yet. The fields match where decentralized networks converged: alt text and aspect ratio (accessibility, stable
layout), explicit spans for mentions and tags (no global handle namespace; hashtags in languages without word
boundaries cannot be parsed reliably), labels readers' filters can act on, and a thread root so a reader need
not walk up parents across relays.

---

## S027 — Every entry ends with an extension section

**Status:** Accepted (2026-10-30)

**Decision:** Every entry ends with a list of `{tag, bytes}` extensions, signed and hashed with the rest.
Readers skip tags they do not know, and an extension may only add information: a reader that ignores it must
still show the entry correctly. Anything that must not be ignored is a new kind or entry version.

**Rationale:** Exact-consume decoding (D088) makes a body closed; the extension section is the one place
additive fields can land without a version bump. Head statements need none: they are reissued, not history.

---

## S028 — `post` stays short; long form is a later `article` kind

**Status:** Accepted (2026-10-30, product)

**Decision:** No title, cover, file names or media roles in `post`. Long form comes later as its own kind
(title, summary, cover and files in the entry; the text in the public CAS realm).

**Rationale:** Networks that carry both keep them apart; a short post's first image is its cover and the mime
type tells inline from attached. A new kind needs no format change today (unknown kinds are kept as stubs,
S016), so it can wait; file names in public posts leak.

---

## S029 — Nothing is sent to another author's relays (v1)

**Status:** Accepted (2026-10-30, product)

**Decision:** A client publishes only to its own account's targets. A relay keeps entries of the authors it
serves and the copies they carry (S022), and refuses others' entries (`not_served`). An author's notices
(replies, mentions, quotes, reposts, likes) come from the logs they already fetch — the people they follow.
Replies from anyone else appear only when a reader opens a thread and taps for more: the client then asks the
root author's and the reader's own relays (a `replies` read over the entries each already holds, with threads
in P3); nothing is fetched for them automatically. Reserved for reconsideration: relays accepting entries that
name the authors they serve, a `refs` read (entries naming an account), an `accepts_notices` term in
`post_relay`, and a price for stranger notices ([pricing](../pricing/) initiation floor).

**Rationale:** Every send path into someone else's relay is a spam and harassment channel. Notices from
follows need none, and the formats that a later notice path would index (`reply_root`, mention facets) are
already signed into posts (S026), so opening it later is a relay policy, not a format change.
