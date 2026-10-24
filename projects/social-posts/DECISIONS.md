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

**Status:** Proposed (2026-10-23)

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
