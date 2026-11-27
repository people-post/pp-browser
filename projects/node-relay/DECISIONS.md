# Node relay — decisions

Rationale for [DESIGN.md](DESIGN.md). Outcomes move to `docs/` when they ship; mark the entry **superseded by** that
doc then.

## NR001 — Two tiers: `pp-node` and `pp-node-plus`, one build for every add-on

**Date:** 2026-11-25; widened 2026-11-27  
**Status:** Accepted (Hongwei, 2026-11-25 as `pp-node-relay`; 2026-11-27: "I like plus", one generic build for every
add-on, the ledger stays in core; services chosen at compile time, the two tiers as the sets we provide, and the
rules for one data folder)  
**Decision:** A node's services are chosen at compile time. We build and release two sets, no third; an operator may
build either binary with another (`-DPP_NODE_SERVICES=…`, `-DPP_NODE_PLUS_SERVICES=…`; `cmake/PpNodeServices.cmake`).
- **`pp-node`, the core:** what every node needs. It helps the mesh (circuit and media relay, DHT, directory) and holds
  nothing for anyone. The ledger role is core, essential for the greater good; it runs only when asked
  (`--ledger-role`), so an operator can leave it off, and a set without `ledger` (or `PP_BROWSER_WITH_LEDGER=OFF`)
  builds without it.
- **`pp-node-plus`, the add-ons:** pp-node plus every **add-on service**, a service that keeps data or acts for
  accounts and that an owner offers (admission, offers, limits, a privacy statement: NR007, NR008, NR011). The message
  relay is the first (the `relay` capability: the mailbox, then the relay protocol's lockers, cards and directory, and
  its call to the org's push gateway); file storage (NR010) is the second. The push gateway itself is the org's (it
  holds the app's APNs / FCM keys), not an add-on.
- One folder per service: in the tree (`capabilities/<service>/`), in the config (`node.<service>.…`, with
  `PP_NODE_<SERVICE>_…` overrides) and on disk (`<profile dir>/<service>/`, beside the ledger role's `ledger/`). Each
  keeps its own store; none reads another's.
- Each add-on is its own capability with its own library and its own on/off setting. A build per add-on would multiply
  binaries, images and releases; an operator turns off what they don't want.
- **A tier is a build decision only.** Nothing on the wire, in a record, on disk, in the config or in a metric names
  it: clients find a service by what a node advertises (its protocols, its directory row), never by its build, and
  `/status`'s `edition` is for operators. A service's code is in its own namespace (`pbr::<service>`), and its library
  defines `PP_NODE_HAS_<SERVICE>`, which starts it in whichever build links it. Moving a service between the tiers is
  moving its name between the two presets in `cmake/PpNodeServices.cmake`. What a move still costs is operational: core →
  plus takes the service away from default nodes at the next node release (release notes, and one release in which the
  default node warns when the service is on); plus → core grows the default node (its size budget) and the service
  must stay off by default there, since a core node never keeps data for accounts unasked.
- **Any set builds.** A service names the services it needs, and a set without them fails configure. Each binary
  links exactly its set: its link allowlist and a symbol check hold it to that. `/status` says which set answers
  (`edition`: `node`, `plus` or `custom`; `services`). CI builds both presets, and the default node with no service and
  with each alone; the size budget is the `node` preset's. We release only the two presets.
- **One data folder, one build at a time.** Different builds may run on the same data folder in turn, never at once
  (two processes would run one identity twice and write the same sealed files):
  1. A build never touches the folder of a service it lacks: no read, no migration, no deletion.
  2. A service that returns catches up on start: its own expiry first (NR007's retention), so nothing is served past
     its time. Each service's store carries its own schema version and refuses a newer one, as profiles do.
  3. What a build lacks is visible: at start it logs that data for services it lacks is kept, not served, and
     `/status` lists them as `dormant`. Clients see a dormant relay as a relay that is down (NR006 sends elsewhere).
  4. Asking for a service the build lacks fails the start, as `--ledger-role` does in a build without the ledger. A
     config section with no explicit ask is ignored with one log line, so one config works across builds.
- Both presets come from the same commit, on the same version line (RC016), each with its own image. The default
  `pp-node` links no add-on; CI checks that, and the default image's size.
- A third build only if an add-on's dependencies, size or security surface are of a different order from the others',
  so that operators who want the light add-ons would otherwise carry it (LLM inference, a full chain node). None is
  planned.

**Rationale:** The default node stays small for everyone who only wants to help the mesh. One commit and one version
mean the two builds never disagree on a format, and one release flow covers both. A name for no one service
(`pp-node-relay` was the first) lets files and the gateway join without another rename.  
Choosing at compile time keeps every combination honest without a plugin interface, and the four rules let an
operator switch builds without losing data.  
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
**Decision:** A node relay (the message relay on a `pp-node-plus`) has an operator set, a relay id and a certificate, and implements the relay protocol's
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
**Decision:** A node relay's mailbox limits are settings in the node's config, each with an environment override as
the node's other settings have, defaulting to: records kept 7 days (a sender's shorter `ttl_s` tier sooner); at most
1,000 records and 16 MiB per account; a node-wide budget from `mesh.capacity`. Past a limit the relay answers `busy`,
with the service's load level (N033). The relay reports its limits in its `relay-identity` answer, so a recipient's app
can say how long its relay keeps mail and a sender's knows what to expect.  
**Rationale:** The `https://` copy carries durability under the default delivery (NR006), so a node relay can stay small
enough for a Raspberry Pi and cheap enough to volunteer; an operator with room, or a group relying on it, raises them.  
**Alternatives:** Fixed limits (one size for a Pi and a server); the HTTPS relay's 90 days as the default (a volunteer's
disk for little gain while both copies exist).

## NR008 — Admission by invite

**Date:** 2026-11-27  
**Status:** Accepted (Hongwei, 2026-11-27, over several rounds)  
**Decision:** A node relay registers an account only with an **invite**: a record signed with the owner's authority
(NR011) — `relay_id`, owner, invitee account, `issued_at`, `expires_at`, extension area — checked by the relay against
its owner's key.
- **The owner's own account** (and its devices) needs none.
- **Friends** get one from the owner's app: "Invite to my relay" on their contact page, sent in the chat.
- **Lifetime and renewal:** 90 days, renewed when 30 are left, by the same sweep as relay grants (on messaging ready, on
  a contacts change, every six hours). Only the owner's app renews: the node does not hold the owner's key.
- **Revocation is immediate:** removing or blocking the friend, or revoking the invite, sends the owner's node a signed
  revoke, and renewal stops.
- **After an invite ends** (revoked or run out): the relay takes no new mail for the account (senders get
  `recipient_not_registered` and go on by the list, as today); mail already waiting stays fetchable until it expires
  (NR007), collect-only; the invitee's app drops the relay from its card once told. **"Remove and delete their mail
  now"** is a separate, deliberate action for abuse.
- **Later, the same invite** is how a paying user gets in (the owner's side issues it once the pricing project's
  payment promise for the offer is settled) and how an org's member or delegate admits someone (NR011's rules).  
**Rationale:** One admission check at the relay whatever the route in: friends now; payment and orgs later only add
ways to receive an invite. Revocation can be immediate because the relay is the owner's, so the expiry is a safety net
and can be long enough that the owner's devices being off for weeks does not cut friends off. Keeping waiting mail is
safe: it is sealed, capped per account, and gone within days; the delete action covers abuse.  
**Alternatives:** Policies on the relay (`self` / `allowlist` / `open`, the earlier proposal): an allowlist the owner's
devices would have to keep in step with the node, and a separate path for payment. Grant-length invites (30 days): the
owner's devices off for three weeks would lock friends out. Deleting waiting mail on revocation by default: punishes
the friend for the owner's choice.

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
the mailbox, as a second add-on of `pp-node-plus` (NR001), with its own configurable limits (file size, total per account,
retention, downloads).  
**Rationale:** Messages and files differ in size, lifetime and cost; one set of limits for both would be wrong for each.
It is already how the HTTPS relay works (its file store keeps files 7 days, 10 downloads, 1 GiB per account).  
**Known gap:** an attachment's link names the sender's relay. A recipient who got the message through a node relay but
cannot reach that relay fetches the file directly from the sender's devices when they are online, and otherwise waits;
node relays' `files` service, and later content-addressed references that name several places to fetch from, close it.  
**Alternatives:** Files inside messages (a mailbox as large as a file store, and every copy under `all` multiplied).

## NR011 — Ownership and offers, ready for org accounts

**Date:** 2026-11-27  
**Status:** Accepted (Hongwei, 2026-11-27)  
**Decision:**
- **Ownership, proven both ways.** The owner's account card lists its relays (a non-critical entry), and the relay's
  certificate names its owner (a signed extension entry). Each side signs its half. The relay's operator key stays apart
  from the owner's account key (one key per job, as pp-ledger-apps' OP004).
- **The offer.** Under the owner's account, signed with its authority: the relay, who may ask (friends only for now;
  anyone later), the limits (NR007), and the price in `pp_credit` minor units (always 0 now; the field exists from the
  start, as the pricing project's currency). A paid subscription later is a payment promise whose `service_ref` names
  the offer; settling it gets the payer an invite (NR008). Friends see offers on the owner's contact page.
- **Three rules that keep org accounts possible:**
  1. every record names its provider by an id (an Account ID now; an org's id later), never as "a person";
  2. a record is checked as **signed with the provider's authority**: the account's key now; later an org's key set
     with a threshold (as relay operator sets) and **delegation**, the org certifying a member or a hot key to sign day
     to day (as a relay certificate certifies a hot signing key). Each record reserves a signed extension entry for that
     chain of authority, so orgs need no format change;
  3. ownership, offers and invites never assume a single owner: an org owning several relays, or a relay owned by an
     org, uses the same records.

Not built now: org accounts, delegation, payment, offers to anyone.  
**Rationale:** Friends come first (Hongwei), payment is designed with the pricing project, and org accounts are on the
horizon: the records should not have to change when they arrive. Proving ownership both ways stops anyone claiming
another's relay, and a relay claiming an account.  
**Alternatives:** The relay's operator set as the owner (an operator key doubling as an account; no place for an
account's other services); org support designed now (no user yet; the three rules keep the door open at no cost).
