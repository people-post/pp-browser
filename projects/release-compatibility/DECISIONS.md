# Release compatibility: decisions

## RC001 — Compatibility within a supported range, from release 1 on

**Date:** 2026-11-17
**Status:** Accepted (Hongwei, 2026-11-17: switch from pre-release to release mode); in review with Leo. When accepted and release 1 ships, it supersedes COMPATIBILITY.md
[§ No bridges (pre-release)](../../docs/contracts/COMPATIBILITY.md#no-bridges-pre-release).
**Decision:** From release 1 on, every app, pp-node and relay works with every other one whose version is inside
the supported range ([DESIGN § The rule](DESIGN.md#the-rule)). Breaking changes may drop only versions leaving the
range. A version older than the range is refused cleanly with "update needed".
**Rationale:**
- The product's parts update independently: phones when their owners choose, pp-node on users' hardware, the relay
  on its own schedule, other operators' relays under multi-relay. A lockstep policy cannot hold on them.
- The pre-release cutovers were a deliberate choice for speed (2026-11-12). They left no rollback and many
  unrelated version numbers, and practice already drifted from them (pp-relay-apis `94c6865`).
**Alternatives rejected:**
- Keep clean cutovers after release (every change strands whoever has not updated; no rollback).
- Support every older version forever (old readers accumulate; the simplicity no-bridges gave is lost).

## RC002 — Format changes roll out readers first, written for the oldest supported version

**Date:** 2026-11-17
**Status:** Accepted (Hongwei, 2026-11-17); in review with Leo.
**Decision:** A format change ships readers of the new form first; then writers switch to the newest form the
oldest version in range reads; the old reader is deleted once nothing in range writes the old form
([DESIGN § The range rule](DESIGN.md#the-range-rule)). Security fixes may shrink the range at once.
**Rationale:**
- Writing for the oldest supported version needs no per-recipient knowledge, so apps need not announce per device
  which formats they read.
- Deleting the old reader when its window ends keeps one path in the code most of the time.
**Alternatives rejected:**
- Each device announces the formats it reads in the account card and senders write per recipient (more state and
  a card field for what the range already gives; may come later as an optimisation).
- Negotiate the highest common version on every exchange without announcing ranges in signed or handshake data
  (strippable: an attacker could force an older version).

## RC003 — The relay can always roll back one release

**Date:** 2026-11-17
**Status:** Accepted (Hongwei, 2026-11-17); in review with Leo.
**Decision:** The relay never writes a record its previous release cannot read; a new record shape is written only
after a release that reads it is deployed. pp-node should follow the same rule. Apps' local data moves forward only.
**Rationale:** On 2026-11-17 a relay revert to the time of TestFlight 136 had no clean answer: the newer relay had
written VoIP device records and version 4 cards that the older one mishandles. The relay is the shared component
whose rollback matters most.
**Alternatives rejected:** roll forward only (every bad deploy becomes an emergency fix).

## RC004 — Release 1 is the floor, shipped together once more

**Date:** 2026-11-17
**Status:** Accepted (Hongwei, 2026-11-17); in review with Leo. Contents decided in phase P0.
**Decision:** Release 1 ships across app, pp-node and relay together, with what the rule needs
([DESIGN § Release 1](DESIGN.md#release-1)): tolerant readers, versions checked first, a clean "update needed",
relay versions and ranges, and a clean start (the relay's legacy window removed, floors moved to release 1,
`min_client_version` raised). Everything before it is outside every range.
**Rationale:** The range rule only holds between versions that carry its basics; a release that lacks them would
have to be bridged forever. Nothing older needs to be supported, so the floor is the cheapest point to add them.
**Alternatives rejected:** apply the rule to today's builds (they refuse unknown content and versions, so the
first format change would break them anyway).

## RC005 — One main branch, a branch per release, every build tagged

**Date:** 2026-11-17
**Status:** Accepted (Hongwei, 2026-11-17). Amended by [RC007](#rc007--the-branch-model-after-leos-review) (2026-11-17,
after Leo's review): tags, environments, build-once and check levels added; backports by a bot. In review with Leo.
**Decision:** Each repo has one `main`; `develop` is retired. Every internal build is tagged from main by the
workflow that makes it. Each release gets a branch `release/X.Y` cut from main, which takes fixes only, cherry-picked
from main, and carries the release tags. The relay deploys prod only from release tags. Libraries keep main plus
tags and cut a release branch only on need ([DESIGN § Branches and tags](DESIGN.md#branches-and-tags)).
**Rationale:**
- Under RC001 several releases are live at once; a release branch is where a fix to one of them goes without
  shipping later format changes, and the live branches are the supported range.
- Today main is develop running behind: it holds no release users run once it moves on, and internal builds leave
  no tag.
- A relay rollback (RC003) becomes a redeploy of the previous release tag.
**Alternatives rejected:**
- Keep develop → main with tags (no place for a fix to an older release; two branches that mean little apart).
- Release tags on main without branches (a fix to release N must ship everything after N).

## RC006 — Range lengths, numbering, tags and fix policy

**Date:** 2026-11-17
**Status:** Accepted (Hongwei, 2026-11-17); items 3 and 8 wait for Leo.
**Decision:** The nine items of [DESIGN § Decisions taken](DESIGN.md#decisions-taken). In short: relay ↔ app
in range for the last 3 app releases and at least 90 days, other pairs the last 2 releases and at least 60 days;
security cutovers signed off by Hongwei; the relay on its own semver; no app downgrade; one version table here;
app and pp-node share numbers, release 1 being 10.0; internal builds tagged `internal/`, `node-internal/`, `qa/`;
the newest release branch takes every fix, older ones security fixes and in-range breaks.
**Rationale:**
- The relay faces phones, which update slowest, so its range is widest; peers of the same kind update closer together.
- 10.0 keeps pp-node's tags rising, so no tag name is reused or goes backwards.
- Fixing only the newest branch fully keeps cherry-picks few while the range stays safe.
**Alternatives rejected:**
- One range for every pair (either too short for phones or longer than peers need).
- Ranges decided by usage share alone (a number nobody controls; it may inform a close, not decide it).
- Restarting pp-node at 1.0 (its tags would repeat 1.x–9.x).

## RC007 — The branch model after Leo's review

**Date:** 2026-11-17
**Status:** Accepted (Hongwei, 2026-11-17); answers Leo's review comment on #634. Amends
[RC005](#rc005--one-main-branch-a-branch-per-release-every-build-tagged).
**Context:** Leo proposed a lightweight Git Flow variant: `develop` as the integration baseline, `main` as the
production baseline taking only complete releases and hotfixes, optional `release/X.Y` stabilization branches,
`hotfix/X.Y.Z` branches from the production tag, back-merges from `main` to `develop` instead of cherry-picks,
immutable `vX.Y.Z` tags as the only production deploys behind GitHub environment protection, artifacts built once
and promoted, and three levels of checks. He asked four questions: (1) optional release branches? (2) merge and
back-merge as the normal propagation? (3) production only from immutable tags with environment protection? (4) the
minimum checks for a hotfix?
**Decision — taken from the review:**
- Immutable `vX.Y.Z` tags; only tags deploy to production, through GitHub environments with an approval gate; for
  apps, only tags go to the stores (question 3: yes).
- Build once and promote the same artifact through candidate, staging / canary and production; rollback redeploys
  the previous tag's artifact.
- Checks by level: feature PRs, release candidates and fixes, tags ([DESIGN § Checks by level](DESIGN.md#checks-by-level);
  question 4).
- The cost of cherry-picks is real: backports are opened by a bot from a label, and keep the cherry-pick trailer.
**Decision — kept from RC005:**
- A release branch for **every** release that ships into the field, kept open while the release is in range
  (question 1: not optional).
- **One** long-lived integration branch (`main`); no separate production branch.
- Fixes land on main first and are **backported**; release branches are not merged back (question 2: backport is
  the normal path, a merge only where it is the same change).
**Rationale for what was not taken:**
- **Several releases are in production at once.** The Git Flow variant fits a service with one deployment: `main`
  is what runs, a hotfix fixes that deployment, and an older version stops existing when the next one is deployed.
  Here it does not: RC006 keeps the last 3 app releases (at least 90 days) in range, on phones and on users' own
  pp-node machines that we cannot update. A security fix to 10.1 while 10.3 is current has no place in the variant:
  hotfixes start from the current production tag, and 10.1 is not on `main` any more. Each in-range release needs
  its own line, so release branches cannot be optional.
- **A second long-lived branch adds a rule to police and no information.** With a branch per release, the release
  tags already record what is in production. A `main` that means "production" would hold only the newest release
  and need the back-merge invariant (`main` always an ancestor of `develop`) enforced forever; a missed back-merge
  silently drops a production fix from the next release.
- **Back-merging does not reach older releases.** A merge from `main` to `develop` carries fixes forward from the
  newest release only. A fix needed on 10.1 and 10.2 cannot come from merging the integration branch into them (that
  would bring every later change, format changes included), so it is a backport either way. Software in the same
  position (several supported versions running where the vendor does not control upgrades) works the same way:
  Kubernetes keeps a branch for each of its last 3 minor releases, Bitcoin Core, Go and Chromium keep release
  branches, and all backport from the main line, usually with a bot.
- **For the relay alone, the two models nearly coincide**: one production at a time, rollback by redeploying the
  previous tag. Its release branch still matters: main may already hold the reader half of a format change
  (RC002) that a hotfix must not carry to production.
**Alternatives rejected:**
- Leo's Git Flow variant as proposed (`develop` + `main` + optional release and hotfix branches, back-merges): it
  maintains only the newest release; reasons above.
- Merging release branches forward (oldest → newer → main) instead of backporting: needs every older branch merged
  into every newer one, and fails when the code has moved on; kept only for a fix that exists only on old branches.

## RC008 — App builds climb one ladder; only release candidates are promoted

**Date:** 2026-11-17
**Status:** Accepted (Hongwei, 2026-11-17).
**Decision:** Dev builds are made by hand from any branch or tag for internal testers and are never promoted. Release
candidates are built automatically from `vX.Y.Z-rc.<n>` tags on `release/X.Y` with their final version, and the same
binary goes from internal to external testers to the store, moved by an approval-gated promotion workflow that
refuses anything else and tags `vX.Y.Z` when it submits. Store releases are phased. Builds show their tag and commit.
([DESIGN § App builds](DESIGN.md#app-builds-internal-external-store).)
**Rationale:**
- It is the stores' own model (one upload, promoted), and RC007's build-once rule.
- The stores cannot tell a dev build from a candidate (a version string and a build number), so the rule has to be
  enforced where the build is promoted, not remembered.
- Under the range rule a version cannot be recalled from phones; a phased release is the one way to stop a bad one
  early.
- On 2026-11-17 "TestFlight 136" could be traced to its commit only by matching workflow run numbers; tags and a
  build that names itself make that immediate.
**Alternatives rejected:**
- Promote any internal build by hand in App Store Connect (the planned flow before this decision): works, but a
  dev build can be promoted by mistake, and the repo does not learn which commit shipped.
- Rebuild for the store from the release tag (the tested binary is not the one shipped).
- A separate dev app now (worth it for mixed-version testing, but its setup is not needed for release 1; later).
