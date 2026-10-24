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

**Status:** Proposed (2026-10-23)

**Decision:** A relay accepts an entry or head because it verifies against the author's key (sent along as a
key card when the relay does not know it); media uploads carry an account-signed presign request. Reads are
unsigned (S011). A relay may additionally require registration on its own terms.

**Rationale:** The signed objects are their own proof of authorship, so anyone may mirror them; quotas key
on the author either way.

---

## S019 — Relays keep forks and stubs, and apply deletes

**Status:** Proposed (2026-10-23)

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
