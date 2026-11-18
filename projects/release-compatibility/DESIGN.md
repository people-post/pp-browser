# Release compatibility: design

**Status:** Accepted by Hongwei, 2026-11-17; in review with Leo (relay parts: [§ Decisions taken](#decisions-taken),
items 3 and 8). Decisions RC001–RC009.
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
run numbers plus 100). Leo's review of the first draft (#634) added the tag, environment, build-once and check-level
rules below; [RC007](DECISIONS.md#rc007--the-branch-model-after-leos-review) records what was taken from it and why
the rest was not.

### Release lines are first-class

The product does not have one production. Apps update when their owners update them, pp-node runs on users'
machines, and under multi-relay relays may be run by others; so "production" is every version still in the
supported range (RC006: the last 3 app releases and at least 90 days), and each of them is a release line that may
need a fix. Branches follow from that:

**One integration branch, `main`.** All feature PRs go to `main`; CI must pass to merge. `develop` is retired in
every repo. (The name is not the point; having one long-lived branch is.)

**One branch per release, `release/X.Y`, cut from main**, for every release that ships into the field: apps,
pp-node, and the relay.

- Release tags (`vX.Y.0`, `vX.Y.1`, …) go on that branch. They are immutable, and release workflows accept no others.
- A release branch takes **fixes only**: no features and no format changes. Additive changes only where a fix needs
  them; a security cutover ([§ Change classes](#change-classes)) may go onto a branch.
- A release branch stays open while its release is in range and closes when the release leaves it: **the live
  release branches are the supported range**. The newest release branch takes every fix; older branches still in
  range take only security fixes and fixes for a break between in-range versions.
- The relay has one prod deployment, but its release branch matters as much: main may already hold the reader half
  of a format change ([§ The range rule](#the-range-rule)) that prod must not get with a hotfix.

**Fixes land on main first, then are backported** to each live release branch that needs them. A bot opens the
backport PRs from a label (for example `backport release/10.1`), and each backported commit keeps the
`(cherry picked from commit …)` trailer, so every fix on a release branch names its original on main. A fix that
only makes sense on an older branch (the code is gone from main) goes straight to that branch, and its PR says why.
Release branches are never merged back into main: main already has every fix.

### Builds, tags and deploys

**Every internal build is tagged** by the workflow that makes it, so every build anyone ran maps to one commit:

| Build | From | Tag |
|-------|------|-----|
| App builds people install (TestFlight, Android, desktop test builds) | main | `internal/<build number>` |
| Release candidates (apps, pp-node) | `release/X.Y` | `vX.Y.Z-rc.<n>` |
| Internal pp-node builds | main | `node-internal/<date>.<n>` |
| Relay dev deploys | main | `dev/<date>.<n>` (with Leo) |

CI builds nobody installs get no tag.

**Build once, promote the same artifact.** A release candidate is built once from its tag; the artifact that passed
is the one released (`commit → build once → test the artifact → staging / canary → production`). This needs
artifacts that do not depend on their environment: the relay takes its settings from the deploy, and an app's
release version is set before its candidate build, so the TestFlight build that passed is the one sent to the store.

**Deploys are gated by GitHub environments:**

- main deploys the relay to dev automatically;
- release candidates from `release/*` deploy to staging (today's qa) ([§ Environments](#environments));
- only `v*` tags deploy to production, behind an approval gate; for apps, only tags go to the stores;
- rollback redeploys the previous release tag's artifact (RC003).

### App builds: internal, external, store

An app build moves along one ladder, and the same uploaded binary climbs it: App Store Connect gives one upload to
internal testers, then external testers, then the App Store, with no rebuild (Google Play's internal → closed →
production tracks promote one app bundle the same way; desktop releases promote the candidate's artifacts). The
rules ([RC008](DECISIONS.md#rc008--app-builds-climb-one-ladder-only-release-candidates-are-promoted)):

| Build | Made from | Goes to | Tag |
|-------|-----------|---------|-----|
| **Dev build** | a manual trigger, any branch or tag (latest features, a fix to try) | internal testers only; **never promoted** | `internal/<build number>` |
| **Release candidate** | pushing `vX.Y.Z-rc.<n>` on `release/X.Y`; built and uploaded to internal testers automatically | internal, then external testers, then the store | `vX.Y.Z-rc.<n>`; `vX.Y.Z` on the same commit when it is submitted to the store |

- **Only release candidates are promoted.** The stores see a version string and a build number only, so a dev build
  stamped with the next version looks like a candidate. A candidate is built with its final version (10.1.0); only
  build numbers differ between candidates.
- **Promotion is a workflow, not clicks.** A workflow adds a candidate to the external testers, then submits it for
  review, through the App Store Connect API (and the Play Developer API). It refuses a build whose tag is not a
  release candidate, runs behind a GitHub environment approval, and tags the release (`vX.Y.Z`) on the candidate's
  commit when it submits.
- **One binary serves both backends.** Builds compile in the prod and staging relays (staging is today's qa origin),
  and testers switch in Me → Network (`backend.env`, prod by default; [CONFIGURATION](../../docs/ops/CONFIGURATION.md)).
  A candidate tested on staging is the
  binary prod users get; it ships with prod as its default.
- **Hotfixes** take the same path from their release branch. A security fix may go from internal testers straight
  to the store with an expedited review, skipping external testers.
- **Store releases are phased.** The App Store's 7-day phased release (and a staged rollout on Play) is used for
  every release, and paused when something goes wrong: a bad version stopped at a few percent is far cheaper than
  one on every phone, since it cannot be taken back.
- **Review times are planned for.** The first build of each version needs Beta App Review before external testers
  get it (usually under a day); later builds of the same version usually do not. The first candidate is cut early.
- **A build says what it is.** The app shows its tag and commit in About and in its diagnostics, so a report names
  the build it came from.
- **Later, after release 1: a separate dev app** ("PP Dev", its own bundle id), so a tester can keep the store
  version and a dev build on one phone for mixed-version testing. It needs a second App Store Connect record, its
  own push and VoIP certificates, app group and keychain.

### Checks by level

| Level | Checks |
|-------|--------|
| Feature PR to main | Build, unit tests, static checks, focused smoke tests (as today) |
| Release candidate on `release/X.Y` | Full CI on every shipped platform, the frozen-data compatibility tests, the mixed-version lab run, the release build |
| Fix or backport to a release branch | All unit tests, the frozen-data compatibility tests, every platform build that ships, the lab scenarios the fix touches (`--only`); for the relay, its integration tests and the local-stack end-to-end test |
| Version tag | No new build: deploy the tested artifact to staging / canary, then production after approval |

The nightly lab and the full lab run on main and before each `.0` release; a fix does not wait for them.

### Libraries and numbering

**Libraries stay simpler.** pp-cpp-* keep main plus version tags. A library release branch is cut only when a
product's release branch needs a library fix without taking a newer major version.

**App and pp-node share a release number.** They come from one repo and share most code, so one `release/X.Y`
produces both, and the range table speaks one number for them. **Release 1 is version 10.0** for both: it continues
pp-node's numbering (v9 today), so pp-node's tags keep rising, and the app moves from 1.0.0 to 10.0. The relay keeps
its own semver (`vX.Y.Z` on its `release/X.Y`) and reports it in `client-compat`; the
[version table](#the-version-table) maps which relay releases serve which app/node releases.

## Environments

Before release mode the relay had two environments: prod, and qa, which took every develop merge and was also the
only place to test before prod. The plan needs three jobs kept apart: integration (the latest main, which may
break), release candidates (the exact artifact to be promoted, tested in prod-like conditions), and production
(rolled out gradually, because a relay deploy reaches every app version at once). Today's qa becomes **staging**,
and **dev** is new ([RC009](DECISIONS.md#rc009--four-environments-todays-qa-becomes-staging-dev-is-new)):

| Environment | What deploys there | Who uses it | Data |
|-------------|--------------------|-------------|------|
| **local** (local-stack and the lab, as today) | any branch, in CI and lab runs | developers, CI | thrown away after each run |
| **dev** (new) | main, automatically | developers, dev builds | may be reset |
| **staging** (today's qa) | release candidates from `release/X.Y`; the previous release for rollback rehearsals | release testing, internal TestFlight candidates | prod-like and kept, no real users |
| **prod** | `v*` tags only, behind an approval, canary first | users and external testers | real |

**Staging keeps everything qa has today**, so nothing on the wire or on disk changes: every app build compiles in
its origin (`PP_API_URL_QA`), users' `config.json` keeps `backend.env: "qa"`, and the relay keeps its PeerId
(`PP_RELAY_PEER_ID_QA`). Only the label in Me → Network becomes "Staging". What changes is what deploys there: release
candidates, no longer every merge.

**Staging is where a candidate is proven:**

- the exact artifact that will be promoted (RC007's build once), with prod-like settings;
- mixed versions: the store app (N−1) and the candidate (N) against the staging relay, beyond the lab;
- a rollback rehearsal: the previous relay release deployed onto data the candidate wrote, proving RC003 before prod
  relies on it;
- migrations and settings: the `client-compat` floor, update links, a window closing.

**Dev is reached without a new build**, through Me → Network's custom relay link (`https://…#<PeerId>`), which
release builds accept when the link names the PeerId. Compiling it in as a third origin can come later if testers
use it often.

**Prod rolls out gradually.** A relay release goes to a share of prod traffic first (Lambda weighted aliases), is
watched, and is rolled back automatically when its alarms fire, the server's version of a phased app release. RC003
makes this safe: while both versions run, each reads what the other writes.

**External testers stay on prod.** They run the candidate app against the current prod relay, the real "new app,
current relay" pair. The relay still ships first: staging, then the prod canary, then the app candidate to external
testers.

**Each environment is separate:** its own tables, buckets, secrets, relay identity (PeerId), LLM upstream token and
pp-node seeds. pp-node gets matching tiers in the ansible inventories (dev, a small staging node or two, prod). Prod
moves to its own AWS account, apart from dev and staging (decision below).

**Cost:** the relay is serverless (Lambda, on-demand DynamoDB), so an idle environment costs almost nothing; the cost
is the staging pp-node machines and the work in terraform, ansible and the workflows.

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
   `dev/<date>.<n>` (relay dev deploys; `qa/` before RC009); none for CI builds nobody installs. *(Leo, for the
   relay's)*
9. **Fixes on release branches:** every fix on the newest; security fixes and fixes for a break between in-range
   versions on older ones in range.

From [§ Environments](#environments) (RC009): agreed by Hongwei on 2026-11-18, **recommended to Leo for review**
(his infrastructure):

10. **Prod's AWS account: prod stays; dev and staging move out.** Moving prod later means migrating live tables,
    buckets, the CDN distribution, secrets and the relay's identity key with users on it. If prod and qa share an
    account today, that account becomes prod-only and a new non-prod account holds dev (built there from the start)
    and staging (moved at the switch, keeping its domain by DNS and its identity key by copying the secret, so apps
    notice nothing). If they are already apart, dev joins the non-prod one.
11. **The prod canary: 10% for 30 minutes, then all** (CodeDeploy's `LambdaCanary10Percent30Minutes`). It rolls back
    on Lambda errors and throttles, the 5xx rate, p99 latency, and a jump in **client-visible error codes** (400,
    401 such as `wrong_audience` or signature failures, 426, 404 `not_registered`): a contract break like the search
    400 of 2026-11-17 shows there, not as a crash. While traffic is low, a scripted smoke run covers the window too:
    relay identity, `client-compat`, lookup, register, send and poll.
12. **Dev's data may be reset at any time**, by a workflow that posts a note when it runs.
13. **Staging starts fresh once at the switch** (best together with its move in item 10), keeping its domain and
    identity: qa's data comes from the pre-release cutovers and is outside every range. From then on it is kept,
    because rollback rehearsals need data a real release wrote. P2 checks that apps re-register by themselves when
    staging answers `not_registered` (not yet verified).
