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
**Status:** Accepted (Hongwei, 2026-11-17); items 3 and 8 wait for Leo. Item 8 amended by
[RC009](#rc009--four-environments-todays-qa-becomes-staging-dev-is-new): the relay's main deploys are tagged `dev/<date>.<n>`.
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

## RC009 — Four environments: today's qa becomes staging, dev is new

**Date:** 2026-11-18
**Status:** Accepted (Hongwei, 2026-11-18); in review with Leo (the infrastructure is his). Amends RC006 item 8.
Its open items (prod's account, the canary, dev's and staging's data) are recommended to Leo in
[DESIGN § Decisions taken](DESIGN.md#decisions-taken), items 10–13.
**Decision:** local (local-stack, the lab), dev (new: main deploys there automatically), staging (today's qa:
release candidates, rollback rehearsals) and prod (tags only, approval, a canary with automatic rollback). Staging
keeps qa's origin, PeerId and the apps' `backend.env: "qa"`. External testers use prod. Each environment has its own
data, secrets and relay identity; pp-node gets matching tiers ([DESIGN § Environments](DESIGN.md#environments)).
**Rationale:**
- One qa did two jobs (integration and pre-production), so a release candidate was never tested apart from the
  latest merges, and nothing rehearsed a rollback.
- Making today's qa the staging environment changes nothing apps hold: the compiled origin, the config value and the
  pinned PeerId stay. Renaming qa to dev would have been a format change to every user's `config.json`.
- A relay deploy reaches every app version at once; a canary is the server's phased release.
**Alternatives rejected:**
- qa becomes dev and staging is new (the first draft): every app build would need a new compiled origin for staging,
  and `backend.env: "qa"` would change meaning.
- Keep prod and qa only (no place for candidates apart from integration).
- Staging as an ephemeral stack per release (loses the kept data a rollback rehearsal needs).

## RC010 — Backups: continuous, on every release and rollback, copied out of prod

**Date:** 2026-11-18
**Status:** Agreed by Hongwei (2026-11-18); recommended to Leo for review (his infrastructure).
**Decision:** Point-in-time recovery on every relay table that holds state (`messages` included, `limits` not),
versioned buckets; an on-demand backup before each prod release, before a rollback and before any data change outside
a release, tagged with the relay version; daily copies into a locked vault in another account (90 days; event
backups while their release is in range); the relay identity key and the Support seed replicated to a second region
and escrowed offline; a quarterly restore rehearsal on staging with a runbook; a removal log re-applied after a restore
([DESIGN § Backups and restore](DESIGN.md#backups-and-restore)).
**Rationale:**
- Continuous recovery loses minutes, a schedule up to a day; the event backups mark the points a release or rollback
  starts from.
- Backups in the account they protect share its failure: a leaked credential or a mistake can delete both.
- The relay identity key is the one thing whose loss no restore repairs: clients pin its PeerId (MR008).
- A restore nobody has practised is a guess; a restore that resurrects deleted accounts is a privacy failure.
**Alternatives rejected:**
- Scheduled backups only (Hongwei's starting point, kept as the daily copies): up to a day of data lost.
- Backups inside the prod account only (no protection from the account itself).
- Keeping backups indefinitely (deleted users' data would live on without bound).

## RC011 — Operating a live release: what release 1 also needs

**Date:** 2026-11-18
**Status:** Agreed by Hongwei (2026-11-18); the relay parts recommended to Leo for review.
**Decision:** Release 1 also carries: aggregate counts of the versions in use; remote switches in `client-compat`
with the app's reader; an app ↔ relay behaviour suite in both repos' CI; relay identity rotation and compromise
recovery; account deletion finished (it exists as *Reset my data…*; corrected 2026-11-18); a crash-report
destination for prod. Release cadence and incidents, notice before
a floor raise, the security process, relays and nodes run by others, and the libraries' share of the range rule are
tracked as not yet designed ([DESIGN § Release 1](DESIGN.md#release-1), [§ Not yet designed](DESIGN.md#not-yet-designed)).
**Rationale:**
- The plan covered formats, branches, builds, environments and backups, not running a release once it is out:
  without version counts a range closes blind; without switches a bad version on phones can only be waited out.
- The switch reader and identity rotation must be in the floor itself: a version shipped without them can never use
  them.
- The search 400 showed formats can match while behaviour does not.
- Account deletion is an App Store requirement, not a choice; *Reset my data…* already revokes and wipes, but does
  not say it deletes the account, and what the relay keeps after a revoke is not stated.
**Alternatives rejected:** adding them after release 1 (the switch reader and identity rotation would then never
reach the versions already out).

## RC012 — A runbook for every operational action, each use recorded

**Date:** 2026-11-18
**Status:** Agreed by Hongwei (2026-11-18); the relay runbooks recommended to Leo.
**Decision:** A release checklist ([DESIGN § Release checklist](DESIGN.md#release-checklist)) and one-page runbooks for
the other actions ([§ Runbooks](DESIGN.md#runbooks)): hotfix, relay rollback, app incident, restore, key compromise
before release 1; range exit and security release before the first window closes; key rotation, the pp-node fleet
rollback and environment resets later. All share one shape (when, who, before, steps as workflow runs, verify,
communicate, follow up). Each use is recorded in a release or incident issue opened from a template. Rollback, restore
and key-compromise recovery are rehearsed on staging.
**Rationale:**
- The rare actions are the stressful ones; no one remembers steps taken twice a year.
- Steps as workflow runs keep runbooks short and leave a record; an issue per use shows what was done and decided.
- Today's checklist (`RELEASE.md`) was written for lockstep pre-release tagging and covers none of the range rule,
  staging, the canary or the phased release.
**Alternatives rejected:**
- One long operations document (hard to follow mid-incident; a page per action is).
- Checklists in a wiki or chat (they drift from the workflows they describe; the repo keeps them beside them).

## RC013 — Regression suites in tiers, picked by capability, run remotely

**Date:** 2026-11-18
**Status:** Agreed by Hongwei (2026-11-18).
**Decision:** Six tiers with time budgets (PR, lab smoke, affected, nightly full, release, after a deploy;
[DESIGN § Regression suites](DESIGN.md#regression-suites)). Scenarios are tagged by capability, and `--changed` picks
them from a branch's diff. Developers start lab runs on the lab machine through the workflow, not on their own
machines. The full run is shortened (timings, split across machines), flaky scenarios are quarantined with an issue,
and the lab machine is set up so the nightly runs at all.
**Rationale:**
- The whole lab is too long to run by hand, so a developer either runs nothing or must know which of 103 scenarios
  matter; tags make the choice from the change itself.
- The nightly had never run (24 of 24 skipped on 2026-11-18: the lab machine's variables were never set): the only
  regression runs were manual.
- Budgets keep a tier usable: a smoke run that grows to an hour stops being run.
- A deploy changes what runs without changing the code; the after-deploy smoke run checks the environment itself.
**Alternatives rejected:**
- Run the full lab on every PR (hours per change, the queue blocks everyone).
- Leave scenario choice to the developer by name (works only for whoever knows the lab).
- Only nightly (a break merged in the morning is found the next day, mixed with everything else merged).

