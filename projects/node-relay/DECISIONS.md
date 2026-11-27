# Node relay — decisions

Rationale for [DESIGN.md](DESIGN.md). Outcomes move to `docs/` when they ship; mark the entry **superseded by** that
doc then.

## NR001 — `pp-node-relay`: a second build of pp-node, not a plugin or a repo

**Date:** 2026-11-25  
**Status:** Accepted (Hongwei, 2026-11-25)  
**Decision:** Relay services are a `relay-services` capability. `pp-node-relay` is pp-node built with it: the same
commit, the same version line (RC016), its own image. The default `pp-node` does not link it; CI checks that, and the
default image's size.  
**Rationale:** The default node stays small for everyone who only wants to help the mesh. One commit and one version
mean the two builds never disagree on a format, and one release flow covers both.  
**Alternatives:** A runtime plugin (no stable C++ binary interface; version skew; loading code to sign and secure). A
relay repo building pp-node through compile-time hooks (Hongwei's first idea): pin skew, a hook API nothing in pp-node
exercises, relay releases lagging pp-node's; with tests and rules it works, and the same-repo build needs none of them.

## NR002 — One relay protocol, two bindings; the Amp binding is `/pp/rpc/relay/1.0.0`

**Date:** 2026-11-25  
**Status:** Accepted (Hongwei, 2026-11-25; the frames 2026-11-27)  
**Decision:** The relay protocol is specified once in pp-specs, apart from transport: each call's signed bytes (sign
v4), objects, refusal codes, vectors. HTTPS stays as it is. Amp carries the same calls as `rpc` frames: a request
`{op, id, body}` with the HTTPS body unchanged, a response `{id, status, code?, body}` with the same status numbers and
codes, and a held `watch` for a live "mail arrived".  
**Rationale:** The calls already sign their relay's transport PeerId; one specification and one set of vectors keep the
TypeScript relay and the C++ one byte-equal. HTTPS remains for serverless relays, networks that block UDP, background
work on phones, and web clients.  
**Alternatives:** Replace HTTPS (re-platform the org relay; UDP-blocked networks; background limits). A protocol of its
own for node mailboxes (the first draft of this project): two protocols for one job.

## NR003 — A node relay is a relay

**Date:** 2026-11-27  
**Status:** Accepted (Hongwei, 2026-11-27)  
**Decision:** A `pp-node-relay` has an operator set, a relay id and a certificate, and implements the relay protocol's
calls with their rules (registration, grants, strangers, cursors, `clear`). No separate "node mailbox" concept.  
**Rationale:** Senders, recipients and the operator tools already know relays; everything they do applies.  
**Alternatives:** Node mailboxes keyed by PeerId with their own subscribe and deposit (first draft; superseded).

## NR004 — Amp relays in a non-critical card entry, `amp_relays`

**Date:** 2026-11-27  
**Status:** Accepted (Hongwei, 2026-11-27)  
**Decision:** The account card lists relays without an `https://` address in a non-critical extension entry
`amp_relays: [{relay_id, hints}]`, beside `relays`, under the same count bounds.  
**Rationale:** `relays` entries must be `https://` URLs and a card breaking a bound is refused whole (MR004), by every
app and relay in the range; a non-critical entry is skipped by all of them.  
**Alternatives:** New `relays` entry forms (cards refused whole by older apps and relays); a new card version (every
reader in the range would need it first).

## NR005 — Relay certificate version 2: no `https://` origin needed with the Amp binding

**Date:** 2026-11-27  
**Status:** Accepted (Hongwei, 2026-11-27)  
**Decision:** Certificate version 2 carries a signed list of bindings and may have zero origins when it lists `amp`.
Version 1 stays as it is for `https://` relays.  
**Rationale:** A relay at home has no domain or TLS certificate, and asking for one defeats the purpose. Only apps that
read `amp_relays` fetch an Amp relay's certificate, so no reader in the range meets version 2 unprepared.  
**Alternatives:** Every relay needs an `https://` origin (a domain and TLS for a Pi at home); a placeholder origin
(breaks the origin rules every reader checks).

## NR006 — With delivery `first`, a send also leaves a copy at the first healthy Amp relay

**Date:** 2026-11-27  
**Status:** Accepted (Hongwei, 2026-11-27), as the default of NR009  
**Decision:** Under delivery `first` (NR009), a send goes to the first `https://` relay that takes it, as today, and also
to the first healthy relay of `amp_relays`. A failed Amp deposit never fails the send. Once the push gateway exists, an
account may list Amp relays only, and then they are sent to as `relays` are.  
**Rationale:** Sending stops at the first relay that takes a message, so an Amp relay first in line would take the
`https://` relay's place, and with it phone wake-ups and the durable copy (N017: peer relays "may never be the
default").  
**Alternatives:** One ordered list for both (phones stop waking when the node relay comes first); Amp relays as a
fallback only (they would rarely get mail, and a home group would gain nothing while the org relay is up).

## NR007 — Light by default, and every limit a setting

**Date:** 2026-11-27  
**Status:** Accepted (Hongwei, 2026-11-27: configurable, with these defaults)  
**Decision:** A `pp-node-relay`'s mailbox limits are settings in the node's config, each with an environment override as
the node's other settings have, defaulting to: records kept 7 days (a sender's shorter `ttl_s` tier sooner); at most
1,000 records and 16 MiB per account; a node-wide budget from `mesh.capacity`. Past a limit the relay answers `busy`,
with the service's load level (N033). The relay reports its limits in its `relay-identity` answer, so a recipient's app
can say how long its relay keeps mail and a sender's knows what to expect.  
**Rationale:** The `https://` copy carries durability under the default delivery (NR006), so a node relay can stay small
enough for a Raspberry Pi and cheap enough to volunteer; an operator with room, or a group relying on it, raises them.  
**Alternatives:** Fixed limits (one size for a Pi and a server); the HTTPS relay's 90 days as the default (a volunteer's
disk for little gain while both copies exist).

## NR008 — The operator decides whom the relay registers

**Date:** 2026-11-27  
**Status:** Proposed  
**Decision:** A registration policy: `self` (the operator's own account and its devices), `allowlist` (named accounts)
or `open` (anyone, within the budget). Default: `self`.  
**Rationale:** The first node relays serve their owner and family; opening one to strangers is a deliberate step.  
**Alternatives:** `open` by default (every node relay becomes everyone's relay at once).

## NR009 — The recipient chooses delivery: `first` or `all`

**Date:** 2026-11-27  
**Status:** Accepted (Hongwei, 2026-11-27)  
**Decision:** An account's card says how contacts deliver to it, in a non-critical extension entry: `delivery: "first"`
(the default: the first `https://` relay that takes it, plus the Amp copy of NR006) or `"all"` (every relay of
`relays` and `amp_relays`). Under `all`, only the first copy accepted carries the push hints (`wake_hint`, `urgent`);
the others are stored without a push. A stranger's send (no grant) always stops at the first relay, whatever the
setting. Apps before the entry send as under `first`.  
**Rationale:** `first` is decided by what the sender can reach; a message then waits on that relay even when the
recipient cannot reach it but could reach another. A recipient whose access changes (networks that block some hosts
at times, UDP blocked at work or in hotels, a home node unreachable from outside) is better served by `all`, where any
one reachable relay delivers. The recipient knows its own networks, so it chooses. The cost is bounded: at most 4
`https://` relays (MR004) and the Amp relays, small records only (files are apart, NR010); duplicates are dropped by
message id; a stranger's price and quota are counted once.  
**Alternatives:** Always `all` (more copies and relay work for everyone who does not need it); the sender chooses (it
does not know the recipient's networks). Later, the app may suggest `all` when one of the account's relays is often
unreachable from its own devices.

## NR010 — Messages carry no files; files are a relay service of their own

**Date:** 2026-11-27  
**Status:** Accepted (Hongwei, 2026-11-27)  
**Decision:** The mailbox carries messages only: anything small (text, reactions, cards, call signalling, roster
changes, grants, attachment references), each within the sealed payload's 128 KiB (`kMaxE2ePlaintextBytes`). Files
travel apart, as they do today: directly between devices (`/pp/blob/1.0.0`) or through a relay's file store, the message
carrying only a reference. The mailbox's limits (NR007) count messages only. On node relays a `files` service comes after
the mailbox, as a second part of `relay-services`, with its own configurable limits (file size, total per account,
retention, downloads).  
**Rationale:** Messages and files differ in size, lifetime and cost; one set of limits for both would be wrong for each.
It is already how the HTTPS relay works (its file store keeps files 7 days, 10 downloads, 1 GiB per account).  
**Known gap:** an attachment's link names the sender's relay. A recipient who got the message through a node relay but
cannot reach that relay fetches the file directly from the sender's devices when they are online, and otherwise waits;
node relays' `files` service, and later content-addressed references that name several places to fetch from, close it.  
**Alternatives:** Files inside messages (a mailbox as large as a file store, and every copy under `all` multiplied).
