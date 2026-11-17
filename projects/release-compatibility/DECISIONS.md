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
**Status:** Accepted (Hongwei, 2026-11-17); in review with Leo (the relay's deploy triggers are his).
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
