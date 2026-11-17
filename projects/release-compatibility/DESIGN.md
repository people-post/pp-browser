# Release compatibility: design

**Status:** Accepted by Hongwei, 2026-11-17; in review with Leo (relay parts: [§ Decisions taken](#decisions-taken),
items 3 and 8). Decisions RC001–RC006.
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
| **Security** | An older version is unsafe to keep accepting | May shrink the range at once: the affected version is refused immediately, still with "update needed". Each one signed off by Hongwei and recorded as a decision with its reason |

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
- a minimum time.

Ranges differ by pair, because phones update slowly (RC006):

| Pair | In range |
|------|----------|
| relay ↔ app | the last 3 app releases, and at least 90 days |
| app ↔ app, app ↔ pp-node, node ↔ node | the last 2 releases, and at least 60 days |

Days count from the release date. What share of users still run a version may inform when a range closes, but
does not decide it.

### Rollback

- **Relay:** must roll back one release without losing data. It never writes a record its previous release cannot
  read. A new record shape is written only after a release that reads it has been deployed.
  A rollback is a redeploy of the previous release tag ([§ Branches and tags](#branches-and-tags)).
- **pp-node:** should roll back one release the same way.
- **Apps:** local data moves forward only; downgrading an app is not supported (phones cannot
  downgrade from the store). A store newer
  than the build is refused with the start-up recovery, as today.

### Versions are announced and checked

- Each format carries a version that the receiver checks **before** anything else, including before verifying a
  signature: a signed object carries its version both inside the signed bytes and readable outside them (the rule
  after #555, today only for envelopes).
- Each component announces its range where it already introduces itself:
  - the relay: `client-compat`, with its release version and supported ranges;
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

One table in `docs/contracts/` (promoted in phase P4) lists every format: its current version, the oldest version
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

**Also:** breaking work already planned or half done. P0's inventory decides which of it lands before the freeze;
the rest waits and follows the range rule.

## Tests

- **Unit tests read frozen data** of the oldest supported version of every format (release 1 at first). This
  extends the relay sign vectors (`relay-sign-vectors.json`) and the envelope sign-bytes hash
  (`EnvelopeSignerTest.SignBytesMatchFrozenHash`). A change to signed bytes without a version bump fails CI.
- **A lab run mixes versions:** the oldest and newest supported app, pp-node and relay, built from released
  artifacts, on the scenarios that cross the changed format.
- Each PR that changes a format says, in the template's *Contracts and docs*: the format, its class, the versions
  on each side, when the old one leaves the range, and the rollback.

## Branches and tags

Under the rule, several releases are live at once, and each one still in range may need a fix (a security fix, a
crash, a relay bug that hits old apps). Today's develop → main flow has no home for a fix to release N once main
has moved on: main is develop running behind, and shipping a fix means shipping everything since N, format changes
included. Internal builds leave no tag either (on 2026-11-17, "TestFlight 136" had to be found by matching workflow
run numbers plus 100).

**One main branch.** All PRs go to `main`; CI must pass to merge. `develop` is retired in every repo.

**Every internal build is tagged from main** by the workflow that makes it: TestFlight, Android and desktop test
builds, internal pp-node builds, the relay's qa deploys. Every build anyone ran maps to one commit:

| Build | Tag |
|-------|-----|
| App builds people install (TestFlight, Android, desktop test builds) | `internal/<build number>` |
| Internal pp-node builds | `node-internal/<date>.<n>` |
| Relay qa deploys | `qa/<date>.<n>` (with Leo) |

CI builds nobody installs get no tag.

**One branch per release, `release/X.Y`, cut from main.**

- Release tags (`vX.Y.0`, `vX.Y.1`, …) go on that branch, and release workflows accept no others.
- A release branch takes **fixes only**: no features and no format changes. Additive changes only where a fix needs
  them; a security cutover ([§ Change classes](#change-classes)) may go onto a branch.
- **Fixes land on main first**, then are cherry-picked to each live release branch that needs them. A fix that only
  makes sense on an older branch says so in its PR. Main never lacks a fix a release has.
- A release branch takes fixes while its release is in range, and closes when the release leaves it: **the live
  release branches are the supported range**. The newest release branch takes every fix; older branches still in
  range take only security fixes and fixes for a break between in-range versions.

**Deploys follow the same shape.** The relay deploys main to qa and only release tags to prod; rollback redeploys
the previous release tag. pp-node releases are tags on a release branch.

**Libraries stay simpler.** pp-cpp-* keep main plus version tags. A library release branch is cut only when a
product's release branch needs a library fix without taking a newer major version.

**App and pp-node share a release number.** They come from one repo and share most code, so one `release/X.Y`
produces both, and the range table speaks one number for them. **Release 1 is version 10.0** for both: it continues
pp-node's numbering (v9 today), so pp-node's tags keep rising, and the app moves from 1.0.0 to 10.0. The relay keeps
its own semver (`vX.Y.Z` on its `release/X.Y`) and reports it in `client-compat`; the
[version table](#the-version-table) maps which relay releases serve which app/node releases.

## Decisions taken

Hongwei, 2026-11-17 (RC006). Items 3 and 8 touch the relay's workflows and wait for Leo.

1. **Range length:** by pair, from the release date ([§ Range length](#range-length)): relay ↔ app the last 3 app
   releases and at least 90 days; app ↔ app and with pp-node the last 2 releases and at least 60 days.
2. **Security cutovers:** each signed off by Hongwei, recorded as a decision with its reason.
3. **Relay versions:** its own semver, `vX.Y.Z` on `release/X.Y`, reported with its ranges in `client-compat`.
   *(Leo)*
4. **App downgrade:** out of scope; local data moves forward only.
5. **Breaking work for release 1:** decided by P0's inventory; whatever misses the freeze follows the range rule.
6. **The version table:** one table in pp-browser's `docs/contracts/`; pp-relay-apis `SPECS.md` links to it and
   lists only the relay's own formats.
7. **One number for app and pp-node:** yes; release 1 is 10.0 for both.
8. **Internal build tags:** `internal/<build number>` (apps), `node-internal/<date>.<n>` (pp-node),
   `qa/<date>.<n>` (relay); none for CI builds nobody installs. *(Leo, for the relay's)*
9. **Fixes on release branches:** every fix on the newest; security fixes and fixes for a break between in-range
   versions on older ones in range.
