# Release compatibility: design

**Status:** Draft for review (Hongwei, Leo), 2026-11-17. Decisions RC001–RC004 are proposed, not accepted.
**Today's policy** stays [COMPATIBILITY.md](../../docs/contracts/COMPATIBILITY.md) until release 1 ships; this
design replaces its [§ No bridges](../../docs/contracts/COMPATIBILITY.md#no-bridges-pre-release) then.

## The rule

> **From release 1 on, every app, pp-node and relay works with every other one whose version is inside the
> supported range.**
>
> - Each kind of component has a supported range of versions: the current release back to the oldest one still
>   supported.
> - Any two components whose versions are both in range work together. A feature only the newer one knows may be
>   unavailable, but the older one says so; nothing fails silently.
> - Breaking changes are still allowed. A breaking change may drop support only for versions that are leaving the
>   range. It never breaks two versions that are both still in it.
> - A version older than the range is refused cleanly, with "update needed".
> - Release 1 is the first build under this rule. Everything before it is outside the range.

### Terms

- **Supported range:** which versions of each kind of component are guaranteed to work together. It is declared
  and published (the [version table](#the-version-table)), not implied.
- **Breaking change:** a change after which the new version cannot work with some older one. Allowed only when that
  older version is already leaving the range.
- **Leaving the range:** a version stops being supported. It is announced, then enforced by raising the relay's
  `min_client_version` (or the node floor).
- **Format:** anything two separately released components exchange or store: a wire message, a signed object, an
  HTTP request or response, a stored record.

## Why

Pre-release, every change was a clean cutover (2026-11-12): no code for older peers, wire forms or data, and every
app and node updated together. That was chosen to move fast, and it worked while one team shipped everything at
once. It does not fit the product:

- Phones update when their owners update them. pp-node runs on users' own hardware. The relay deploys on its own
  schedule, and multi-relay means relays run by others. These parts never move in lockstep.
- The repo has many version numbers (below), each designed carefully but alone. Nothing says which combinations of
  app, node and relay work together.
- No component can roll back: every cutover writes data its previous release refuses (account card v4,
  `profile.db` 7 → 8). A relay revert to the time of TestFlight 136 (2026-11-17) had no clean answer.
- Mismatches surface inconsistently. Some say "update" (relay `426`, an unknown `envelope_version`); others look
  like failures or forgeries (sign v4, [#555](https://github.com/people-post/pp-browser/issues/555); capability
  offers each side refuses; unparsable addresses).
- Practice already drifted: the relay serves apps older than `sign_version` until 2026-12-31 (pp-relay-apis
  `94c6865`), a bridge the policy says does not exist.

## Scope

The rule covers every format between separately released components:

| Pair | Formats |
|------|---------|
| app ↔ app | envelope, account card, control lane, contact cards and locators, group bundles |
| app / pp-node ↔ relay | signed relay calls (`sign_version`), the card at registration, responses and error codes, `client-compat` |
| app ↔ pp-node, node ↔ node | Amp wire, addresses (`/adp/N/`), LAN discovery, the channel-0 capability offer, mesh protocol ids, media frames, DHT and directory records |
| stored data | the relay's tables and files; each device's local stores; the node's local state |

The pp-cpp-* libraries have no range of their own: they are compiled into the products, and the products' ranges
cover what they put on the wire.

## Rules

### Change classes

| Class | What it is | What it takes |
|-------|------------|---------------|
| **Additive** | A new optional field, content type, control type, error code or protocol id that older readers can ignore | No version change. Readers ignore what they do not know |
| **Format change** | A change older readers cannot ignore | A new version, rolled out by the [range rule](#the-range-rule) |
| **Security** | An older version is unsafe to keep accepting | May shrink the range at once: the affected version is refused immediately, still with "update needed". Decided case by case (open decision 2) |

### The range rule

A format change rolls out in three steps:

1. **Readers first.** A release reads the old and the new form. It ships before anything writes the new form.
2. **Then the writer switches.** A release writes the newest form that **the oldest version in range** can read.
   Senders do not need to know what each recipient reads: writing for the oldest supported version is enough.
3. **Then the old reader goes.** Once the last version that writes the old form has left the range, the reader of
   the old form is deleted. Old forms do not accumulate.

Steps 1 and 2 may be the same release when nothing in range writes the old form any more.

### Range length

A version stays in range at least as long as the longest of:

- the life of the release after it (a version leaves only after its successor has been out for a while);
- the life of stored data it writes (relay inbox retention, kept cards, chat history);
- a minimum time (open decision 1).

Ranges may differ by pair. Phones update slowly, so the relay likely accepts a wider range of apps than apps
need of each other (open decision 1).

### Rollback

- **Relay:** must roll back one release without losing data. It never writes a record its previous release cannot
  read. A new record shape is written only after a release that reads it has been deployed.
- **pp-node:** should roll back one release the same way.
- **Apps:** local data moves forward only; downgrading an app is not supported (open decision 4). A store newer
  than the build is refused with the start-up recovery, as today.

### Versions are announced and checked

- Each format carries a version that the receiver checks **before** anything else, including before verifying a
  signature: a signed object carries its version both inside the signed bytes and readable outside them (the rule
  after #555, today only for envelopes).
- Each component announces its range where it already introduces itself:
  - the relay: `client-compat`, plus its release version (open decision 3);
  - pp-node and apps on a link: the channel-0 capability offer (it already carries the wire version, node version
    and features, N032);
  - apps and nodes to the relay: `sign_version` on every signed call, as today.
- A range announced inside a signed object or a handshake is covered by its signature or transcript, so it cannot
  be stripped to force an older version.
- Every refusal caused by a version names who needs to update: the relay's `426` with `upgrade_urls`, an
  "update to view" placeholder for a message, a peer marked as needing an update. A version mismatch never looks
  like a forgery or a network failure.

### Readers tolerate what they do not know

- JSON and records: unknown keys are ignored everywhere.
- An unknown content type, a higher payload version or a higher `envelope_version` is skipped with "update to
  view", and its sequence number is still consumed so sync does not stall.
- Unknown control types, mesh protocol ids, capability-offer features and relay error codes are ignored, or read
  as a plain refusal with a reason. Never a stall or a loop.
- Crypto stays strict: a signature, AEAD or decryption failure is a hard refusal.

## The version table

One table in `docs/contracts/` (promoted in phase P3) lists every format: its current version, the oldest version
readers accept, its owner, and the release at which its oldest version leaves the range. It replaces the
"Clean cutovers" table in COMPATIBILITY.md, which stays as history ending with release 1.

Starting inventory, to be checked in phase P0 (from COMPATIBILITY.md and the 2026-11-17 review):

| Format | Version today | Owner |
|--------|---------------|-------|
| Envelope / sign | `envelope_version` 3, sign v5 | pp-browser (delivery) |
| Account card | 4 | pp-browser (people), pp-relay-apis |
| Relay grant | `pp-browser:relay-grant-v1` | pp-browser (delivery), pp-relay-apis |
| Contact card / locator | `sub_version` 3, `pp-browser-contact-v1:` | pp-browser (people) |
| Relay signed calls | registration v3, relay API v3, others v2 (`sign_version`) | pp-relay-apis |
| `client-compat` | `min_client_version` / `latest_client_version` (app only) | pp-relay-apis |
| Amp wire | 2 (version byte) | pp-cpp-amp |
| Addresses | `/adp/2/` | pp-cpp-amp |
| LAN discovery | mDNS TXT `wire=` | pp-cpp-amp |
| Channel-0 capability offer | leading byte `4` | pp-cpp-amp, pp-cpp-mesh |
| Media frames | 4 (extension block) | pp-cpp-mesh |
| Mesh protocols | version in each protocol id | pp-cpp-mesh, pp-browser |
| Relay tables | no record versions; card version inside | pp-relay-apis |
| Local stores | `profile.db` 8, `thread.db`, `contacts.json`, `identity.enc`, `our_relays.json`, `relay_pins.json`, … | pp-browser |

## Release 1

Release 1 is the floor of every range. It ships across app, pp-node and relay together, the last time anything
ships in lockstep. Phase P0 turns this list into exact work items.

**Needed, because the rule cannot hold without them:**

- Readers that tolerate what they do not know ([above](#readers-tolerate-what-they-do-not-know)). Today ingest
  often hard-refuses an unknown content type or envelope version, and `ChatHistoryRequest` refuses unknown keys.
- A version every receiver checks first, for every signed object, not only envelopes.
- A clean "update needed" for every version refusal.
- The relay with release versions it reports, and its range in `client-compat`; a node floor beside the app's.
- A clean start: the relay's legacy window for older apps (pp-relay-apis `94c6865`) removed, pre-release readers
  and local-store migration steps below release 1 dropped (the floors move to release 1), and `min_client_version`
  raised to release 1.

**Worth doing now, because they are cheapest before anything older exists** (each could also come later through
the range rule):

- A signed extension area in each signed object (envelope, card, relay calls, grants): covered by the signature,
  unknown entries ignored. Today unknown envelope keys are not signed, so each new signed field is a format change.
- An extension block in each binary format that lacks one (media frames have one).
- Every key, signature, KEM and AEAD blob names its algorithm, so a later algorithm is a new version, not a new
  identity.

**Also:** breaking work already planned or half done that should land before the floor is set (open decision 5).

## Tests

- **Unit tests read frozen data** of the oldest supported version of every format (release 1 at first). This
  extends the relay sign vectors (`relay-sign-vectors.json`) and the envelope sign-bytes hash
  (`EnvelopeSignerTest.SignBytesMatchFrozenHash`). A change to signed bytes without a version bump fails CI.
- **A lab run mixes versions:** the oldest and newest supported app, pp-node and relay, built from released
  artifacts, on the scenarios that cross the changed format.
- Each PR that changes a format says, in the template's *Contracts and docs*: the format, its class, the versions
  on each side, when the old one leaves the range, and the rollback.

## Open decisions

1. **Range length:** a number of releases, a minimum number of days, or both; measured from the release date or
   from what share of clients still run it. Per pair (relay ↔ app likely wider).
2. **Security cutovers:** whether each needs Hongwei's sign-off.
3. **Relay versions:** release tags on pp-relay-apis main, as pp-node does, reported by the relay.
4. **App downgrade:** out of scope (assumed).
5. **Breaking work for release 1:** what is in flight that should land before the floor is set.
6. **Where the version table lives** for the relay: here in `docs/contracts/` with a pointer from pp-relay-apis
   `SPECS.md`, or one table in each repo.
