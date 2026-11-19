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
Amended again by [RC014](#rc014--backport-and-hotfix-rules-after-leos-second-review).
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
**Status:** Accepted (Hongwei, 2026-11-17); items 3 and 8 wait for Leo. Item 7 superseded by
[RC016](#rc016--app-pp-node-and-relay-each-have-their-own-version) (each product its own version). Item 8 amended by
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
[RC005](#rc005--one-main-branch-a-branch-per-release-every-build-tagged). Amended by [RC014](#rc014--backport-and-hotfix-rules-after-leos-second-review)
(the backport and hotfix rules).
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
**Status:** Accepted (Hongwei, 2026-11-17). Amended by [RC015](#rc015--client-side-answers-after-kenneths-review): store builds compile in prod only.
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
**Status:** Accepted (Hongwei, 2026-11-18); in review with Leo (the infrastructure is his). Amends RC006 item 8. Amended by [RC015](#rc015--client-side-answers-after-kenneths-review): store builds compile in prod only.
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

## RC014 — Backport and hotfix rules after Leo's second review

**Date:** 2026-11-18
**Status:** Accepted (Hongwei, 2026-11-18); answers Leo's second review comment on #634, which agreed with RC007's
direction (one integration branch, a release branch for every field release in range, immutable tags with
tag-only production, build once, automated backports). Amends
[RC007](#rc007--the-branch-model-after-leos-review).
**Decision:** All of Leo's proposals, written into [DESIGN § Release lines](DESIGN.md#release-lines-are-first-class):
- `main` is stated as the integration baseline, not production; production is the release tags and live branches.
- A backport label makes a bot open a PR to that exact release branch, which runs that branch's checks and needs a
  human approval.
- Backports keep the source commit trailer and link the original main PR.
- A cherry-pick that does not apply is reported (a draft PR or a comment on the original), never adapted silently;
  an adaptation is a separate commit marked "branch-specific backport".
- A release-branch-only fix carries an exception label and says why main needs no equivalent.
- The release owner closes a branch when its release leaves the range; open backports are closed with a note; the
  branch is protected and kept.
- A hotfix starts from the affected release branch, runs that line's checks, gets a patch tag there, and takes the
  same build-once promotion.
**Rationale:** These make RC007 operational: the mechanical cherry-pick leaves developers' normal work while each
release line keeps its own validation and an audit trail of which fixes reached it; the hotfix rule makes the
supported range actionable for older app versions, not only the newest.
**Refinements in accepting:** a bot cannot open a PR from a cherry-pick that fails, so a conflict is a draft PR with
the conflict described, or a comment on the original PR; and the branch-closing steps (who, open backports, protect
and keep) are ours, as Leo asked for them to be defined.
**Alternatives rejected:** none of Leo's proposals.

## RC015 — Client-side answers after Kenneth's review

**Date:** 2026-11-19
**Status:** Accepted (Hongwei, 2026-11-19); answers Kenneth's review on #634. Its "release owner for 10.0" reads
"for release 1" since [RC016](#rc016--app-pp-node-and-relay-each-have-their-own-version). Amends
[RC008](#rc008--app-builds-climb-one-ladder-only-release-candidates-are-promoted) and
[RC009](#rc009--four-environments-todays-qa-becomes-staging-dev-is-new).
**Decision:**
- **Version table:** the control lane, group chat messages and bundles, attachment metadata, the account backup format,
  and the support and AI client contracts join the inventory, checked in P0; preferences go under local stores.
- **Local stores:** stores only the same install reads migrate forward only and are exempt from readers-first; data
  another install reads (the account backup, device names, linked-device sync) is not.
- **Store builds compile in prod only;** testers reach staging on a candidate through a custom relay link; dev builds
  keep prod and staging. No Staging row reaches store users.
- **Dev builds** are cut by hand from any branch or tag (the tag table said main; RC008 said any branch: fixed);
  builds a developer installs on their own device, and CI builds, get no tag.
- **Branch switch:** develop → main happens first in P2, every repo in the same week; no fixed date.
- **Reviews:** every PR into main needs one approval from someone other than its author, by branch protection.
- **Release owner for 10.0:** Hongwei; the role rotates after.
- **Update-needed screen:** its text and translations are Kenneth's; prod's update links Leo's.
- **Runbooks:** a minimum OS version raise is weighed in the range exit; a store rejection mid-rollout is an app
  incident.
**Rationale:**
- A store a single install reads has no other reader, so readers-first there only accumulates old UI readers; the
  exemption stops at data that crosses devices, where other releases do read it.
- One binary for both backends would show every store user a Staging row; prod-only candidates keep build-once and
  keep test servers out of users' menus, as Leo had asked.
- An author approving their own change is no review; with three people one other approval stays light.
**Alternatives rejected:**
- Readers-first for local stores too (old readers carried for nothing).
- A Staging row in store builds, hidden behind a developer switch (still compiled in; a custom link does the same
  for testers without it).
- A fixed switch date now (P2's start depends on #634's review).

## RC016 — App, pp-node and relay each have their own version

**Date:** 2026-11-19
**Status:** Accepted (Hongwei, 2026-11-19). Supersedes RC006 item 7 (app and pp-node share numbers, release 1 = 10.0).
**Decision:** Three version lines: the app (`vX.Y.Z`, branches `release/app-X.Y`, release 1 = **1.0**), pp-node
(`pp-node/vX.Y.Z`, branches `release/node-X.Y`, release 1 = **10.0**, the next major after v9) and the relay
(`vX.Y.Z` in pp-relay-apis, branches `release/X.Y`, release 1 = **1.0**). The version table lists, per release of
each product, the format versions it writes and reads ([DESIGN § Libraries and numbering](DESIGN.md#libraries-and-numbering)).
**Rationale:**
- Compatibility is decided by format versions, not product numbers; a product number only names a release, so
  independent numbers cost one more table, not a weaker rule. (RC006 item 7 chose a shared number to keep the range
  table readable; the per-format table makes that unnecessary.)
- The three ship on different cadences: store review, the node fleet and user-run nodes, relay deploys. A shared
  number forces a release of both for a fix in one.
- The repo already releases app and node on separate trains (`v*`, `pp-node/v*`); a shared number would merge them.
- The app's first public release reads 1.0 instead of jumping to 10.0.
**Alternatives rejected:**
- One number for app and pp-node (RC006 item 7): see above.
- A shared minor line with independent patches (`release/10.1` producing app 10.1.z and node 10.1.z'): ties feature
  cadence together while gaining little over fully independent lines.
**Cost:** a fix in code both products share is backported to each product's branch that needs it (a label each).

## RC017 — P0's decisions: a clean local start, the currency id fixed, two features after release 1

**Date:** 2026-11-19
**Status:** Accepted (Hongwei, 2026-11-19). Answers [INVENTORY](INVENTORY.md) C5 and D1–D3.
**Decision:**
1. **Local stores start clean at release 1.** Every tester deletes the app and installs release 1 (they are in direct
   contact). Each store's floor becomes its current version and every pre-release migration step goes, including
   `identity.enc` 3→4 and `contacts.json` 2→3; no reset path is added for stores below the floor (INVENTORY C4).
2. **`pp_credit` is the currency's protocol id, fixed for good.** It is already on the wire (billing control details),
   inside signed payment promises, in two local stores, and the implied unit of the card's `initiation_floor`. The
   name people see (`kPricingCurrencyDisplayName`) stays free to change. **The minor unit is one credit:** every amount
   counts credits, as a 64-bit integer (`int64_t`; the card's floor is capped at 2⁵³−1 so the relay reads it exactly),
   never a floating-point number, and a rate names the unit it is per (INVENTORY C6). A major unit, decided later,
   only groups credits for display; amounts on the wire, in signed bytes and in stores do not change. Readers refuse or ignore an amount in a currency they do not know (INVENTORY T18). A
   second currency later is a new id, and a currency field beside `initiation_floor`: additive.
3. **The easy recovery tier (seed in the synced keychain, multi-device-account M023 §3) comes after release 1.** It is
   additive: an app without its reader asks for the phrase. When built, the synced item carries a version and has
   frozen data from day 1, because another install on another version reads it.
4. **N031's signed node records come after release 1.** Directory row readers already ignore unknown keys, so records
   arrive additively; the relay keeps accepting heartbeats and clients read rows and records for the node pair's range
   (2 releases, 60 days), and the node floor (INVENTORY A2) closes the old path.
**Rationale:**
- With every tester reinstalling, nothing older than release 1 exists, so steps kept for older files would never run.
- Renaming a protocol id later is a format change on the wire, in signed bytes and in stores; a display name is not.
- Both features are additive under the range rule, and neither is built (N031 is L-sized), so neither is worth
  delaying release 1.
**Cost:** release 1's default "Just continue" path keeps no off-device copy of the seed: a user who never shows their
phrase or saves a copy loses the account with the phone. Two paths stay open for 60 days after N031 ships.

## RC018 — The release-1 answers for Kenneth's and Leo's rows

**Date:** 2026-11-19
**Status:** Part 1 accepted (Hongwei, 2026-11-19). Part 2 is Kenneth's and Leo's to decide; until they do, its default
stands, so no release work waits on it. Answers the questions [INVENTORY § 3](INVENTORY.md#3-rows-from-rc015-kenneth-and-for-leo)
leaves for them.

**Part 1 — decided now** (each sets a format, a reader that ships in release 1, or the order of release work):
1. **Two deletion actions** (INVENTORY O5). *Delete account…* revokes the registration on every relay of ours, deletes
   the account's relay data including its lockers (item 8), and wipes this device; it says plainly that it deletes the
   account on every device and cannot be undone, even with the recovery phrase. *Remove this device…* wipes only this
   install and revokes nothing; on the main device it also takes the device off the card, and on a linked device it is
   the missing "leave". Registration stays one per Account ID.
2. **The AI client follows the relay's current contract** (C1): `llm_token` renewed before it expires, the relay's
   error codes (`user_rate_limit`, `user_daily_limit`, `guest_*`, `token_expired`) instead of message text, and
   "registered or guest" from the key's slot (`brief_llm_api_key` / `brief_llm_guest_api_key`), not its prefix.
3. **The relay's version** (RC006 item 3, A1): its own semver `vX.Y.Z` on `release/X.Y`, starting at 1.0, reported in
   `client-compat` at `schema_version` 1 as `relay_version`, `app_range` and `node_range`.
4. **Floors at release** (A2, C3): `min_client_version` `1.0.0`, and a new `min_node_version` `10.0.0`.
5. **The legacy parts go in relay 1.0** (C2), deployed with app 1.0: the sign window, the `blobs/*`, `files/*` and
   `GET /search` aliases, `llm_api_key` and `keys/rotate`, the pre-cutover sender mapping. Testers reinstall that day
   (RC017), so no supported app needs them and no window is kept.
6. **The version header** (O1): `X-PP-Client: <product>/<version> <os>` on every relay call from the app and pp-node.
7. **Relay identity rotation** (O4): a cold rotation key, kept offline and escrowed with the backups (RC010), whose
   public key builds carry beside the relay's PeerId; it only signs "this relay's identity is now X", which also covers
   a leaked identity key. A planned rotation may in addition be signed by the old identity key. The reader ships in
   release 1, since a build without it can never follow a rotation.
8. **What a revoke deletes** (O5): the mailbox (messages, META, large bodies), the public icon and its record, the quota
   counter, every push row (a lost purge retried), and the lockers; an expired registration is cleaned of the same set
   on a schedule. Support conversations are kept for a stated period, then deleted (the period: part 2).
9. **Relay codes and fields the app reads** (T8, U10, O2): the account card stored as received; `relay_upgrade_required`
   for a card or `sign_version` newer than the relay reads; remote switches as `switches: {name: true | false}` in
   `client-compat`.

**Part 2 — Kenneth's and Leo's to decide, default until then:**

| Item | Owner | Default |
|------|-------|---------|
| Update-needed display and words (U1, U2, U11, T14) | Kenneth | one persistent banner with an Update button, a full screen only below `min_client_version`; a per-contact line in the chat header; "relay needs an update" apart from "update the app"; en and zh-Hans |
| `client-compat` `message` vs translations (U12) | Kenneth | the app's own translated text always; the server's `message` as an extra line |
| Both deletion confirmations' text (item 1) | Kenneth | plain words as in item 1 |
| *Delete account…* offline | Kenneth | needs a connection: offline it says so and wipes nothing |
| Crash-report opt-in (O6) | Kenneth | a switch in Me, off by default, one line on what is sent |
| Person-level preferences in the locker | Kenneth | none in release 1 (additive later, once INVENTORY T7 lands) |
| Where versions are counted (O1) | Leo | a CloudWatch metric by product, version and OS; no account id, no table |
| How `RELAY_VERSION` reaches the functions; an `x-pp-relay-version` header | Leo | injected by the deploy workflow from the tag; header optional |
| Deploys (RC006 item 8, X11) | Leo | `dev/<date>.<n>` tags; a Lambda version per tag and one alias per function |
| Checks before deploy (O3) | Leo | `deploy` needs `integration`; the behaviour suite on PRs in both repos; the nightly lab's variables set |
| Prod's update links (C3) | Leo | the App Store and Play links and the desktop download page, set on the release day |
| Support retention, S3 lifecycle days (item 8) | Leo | 90 days each, stated in SPECS and the privacy text |
| The crash-report endpoint (O6) | Leo | as SPECS plans: a bucket with a 90-day expiry, no account id, rate-limited |
| The support-worker's "not readable" reply (X9), wake payload version (X10), PITR on every state table | Leo | yes to each |

**Rationale:** Part 1 is what code on both sides is written against, or what decides when the relay's old paths can
go; leaving it open would stall P3. Part 2 changes no format the other side reads, so it can be settled during P3.
