# Release compatibility: phases

Order of work. Status is in [CURRENT_STATE.md](CURRENT_STATE.md).

## P0 — Inventory

Written up in [INVENTORY.md](INVENTORY.md) (2026-11-19); its work list is agreed in review before P1.

- [x] Every format in [DESIGN § Scope](DESIGN.md#scope), per repo (pp-browser, pp-cpp-amp, pp-cpp-mesh, pp-cpp-common,
  pp-cpp-crypto, pp-relay-apis): its version, where the receiver checks it, what an unknown version or field does
  today.
- [x] Each checked against [DESIGN § Release 1](DESIGN.md#release-1); the result is the list of release-1 work items,
  needed and worth-doing marked apart.
- [x] The breaking work in flight: which lands before the freeze, which waits for the range rule.
- [x] Kenneth's additions to the version table checked (RC015): the control lane, group messages and bundles, attachment
  metadata, the account backup format, the support and AI client contracts; preferences under local stores.
- [ ] Leo's review of the relay's parts ([DESIGN § Decisions taken](DESIGN.md#decisions-taken), items 3 and 8).

## P1 — Freeze

**In force since 2026-11-19** (Hongwei), until release 1 ships.

- [ ] No new breaking work outside the P0 list until release 1 ships. The list is [INVENTORY § 4](INVENTORY.md#4-release-1-work-list)
  and the planned work it sends before the freeze ([§ 5](INVENTORY.md#5-breaking-work-in-flight)); anything else is
  additive (old readers ignore it) or waits for the range rule. A PR that breaks a format names its INVENTORY item in
  *Contracts and docs*. The rule covers pp-browser, pp-relay-apis and the pp-cpp-* libraries.

## P2 — One main, release branches

[DESIGN § Branches and tags](DESIGN.md#branches-and-tags), in every repo, before release 1 is cut:

- [x] **First, at the start of P2:** develop merged into main a last time; open PRs retargeted to main; develop deleted
  (pp-browser, pp-cpp-*, pp-ledger; pp-relay-apis with Leo), all repos in the same week; P2's own work lands on main.
  Done 2026-11-20 in all ten repos (pp-browser, the five pp-cpp-*, pp-ledger, pp-ledger-apps, pp-relay-apis,
  pp-relay-infra-ansible); the relay repos deployed qa from main and prod from `v*` tags (qa and main deploys superseded by [RC021](DECISIONS.md#rc021--the-relay-repos-deploy-by-tag-only): tags only). Started 2026-11-19 ([RC019](DECISIONS.md#rc019--p2-starts-one-main-the-relay-deploys-from-main-and-tags-no-protection-on-the-private-relay-repos)): the relay repos move once their deploys do.
- [x] Branch protection on main in every repo except pp-relay-apis and pp-relay-infra-ansible ([RC019](DECISIONS.md#rc019--p2-starts-one-main-the-relay-deploys-from-main-and-tags-no-protection-on-the-private-relay-repos) item 3): one approval from someone other than the author, code owners on their
  paths. Done 2026-11-20: main in the eight repos (admins may bypass; CI checks required, pp-browser's one `required`
  check); pp-browser's `release/**` ruleset (one approval, the `required` check, no force push or deletion).
- [x] Every internal build tagged by its workflow: TestFlight, Android and desktop test builds, release candidates,
  internal pp-node builds, the relay's dev deploys. Done 2026-11-20 (`internal/<n>`, `rc-build/<n>`,
  `node-internal/<date>.<n>`, relay `dev/<date>.<n>`); there is no Android or desktop test build people install yet.
- [x] Release workflows take release tags only from `release/*` branches, build once, and promote the artifact.
  Done 2026-11-20: app and pp-node (`release/app-X.Y`, `release/node-X.Y`), relay (`release/X.Y`, Hongwei's tags).
- [ ] GitHub environments, by tag only ([RC021](DECISIONS.md#rc021--the-relay-repos-deploy-by-tag-only)):
  `dev/<date>.<n>` → dev; `release/vX.Y.Z-rc.<n>` → staging; only `v*` tags → production, behind an approval.
  So far: the relay's main → qa and `v*` tags → prod (only Hongwei); the relay repos move to tags, and dev and
  staging wait for Environments below.
- [x] The backport bot ([DESIGN § Release lines](DESIGN.md#release-lines-are-first-class), RC014): a label opens a PR
  to that exact release branch, running its checks and needing approval; trailer and link to the original kept;
  conflicts reported as a draft PR or comment; an exception label for release-branch-only fixes; branch closing
  (open backports closed with a note, the branch protected and kept). Done 2026-11-20 in pp-browser and pp-relay-apis.
- [ ] App builds ([DESIGN § App builds](DESIGN.md#app-builds-internal-external-store)): release-candidate tags build
  and upload automatically; the promotion workflow (App Store Connect and Play APIs, approval-gated, refuses non-candidates,
  tags `vX.Y.Z`); phased store releases; the app shows its tag and commit in About and diagnostics. So far:
  candidates build and upload, and the app names its build; the promotion workflow (needs App Store Connect and
  Play API keys), phased releases and an Android build number (`versionCode` is 1) are left.
- [ ] The check levels in CI ([DESIGN § Checks by level](DESIGN.md#checks-by-level)). So far: PRs to a release
  branch build every platform and run every test; the frozen-data tests (P3) and the mixed-version lab run are left.
- [ ] Environments ([DESIGN § Environments](DESIGN.md#environments), Leo's infrastructure): dev created (relay,
  its PeerId, secrets, tables); qa becomes staging (takes release candidates only; Me → Network shows "Staging", the
  config value stays `qa`); prod canary deploys with alarms and automatic rollback; pp-node dev and staging tiers;
  a rollback rehearsal on staging.
- [ ] Backups ([DESIGN § Backups and restore](DESIGN.md#backups-and-restore), Leo's infrastructure): point-in-time
  recovery on every state table; backups before each prod release and rollback in the workflows; the locked vault
  copy in another account; the identity key and Support seed replicated and escrowed; the removal log; the restore
  runbook and a first restore rehearsal on staging.
- [x] Release and incident records ([DESIGN § Release checklist](DESIGN.md#release-checklist), [§ Runbooks](DESIGN.md#runbooks)):
  the `release` and `incident` issue templates; the workflow that opens a release issue when a product's release branch is cut.
  Done 2026-11-20 in pp-browser and pp-relay-apis.
- [ ] The runbooks needed before release 1, each a page in `docs/ops/runbooks/` with its steps as workflow runs:
  release, hotfix, app incident (pp-browser); relay rollback, restore, key compromise (pp-relay-apis, Leo). So far:
  pp-browser's three (2026-11-20); the relay's are Leo's.
- [ ] Regression suites ([DESIGN § Regression suites](DESIGN.md#regression-suites)): the lab machine set up (its repo
  variables and AWS role) so the nightly runs; scenarios tagged by capability, `--tags` and `--changed`; the lab-smoke
  set on every merge to main; the after-deploy smoke run in the relay's deploys. So far: tags, `--tags`,
  `--changed` and lab-smoke on main (2026-11-20; idle until the lab machine is set up); the lab machine and the
  relay's after-deploy smoke are left.
- [x] The PR template and `docs/ops/RELEASE.md` say what a release branch takes, that fixes land on main first, and
  how a backport is asked for.

## P3 — Release 1 built

- [ ] Each P0 item, repo by repo; the relay first (dev, then staging), as for every relay change.
- [ ] Frozen data of each release-1 format checked in, read by unit tests in each repo.
- [ ] RC011's release-1 items ([DESIGN § Release 1](DESIGN.md#release-1)): version counts at the relay; remote
  switches in `client-compat` and the app's reader; the app ↔ relay behaviour suite in both CIs; relay identity
  rotation; account deletion finished (named as such, relay data after a revoke, offline resets, several devices);
  a crash-report destination.

## P4 — Release 1 shipped

- [ ] Release 1's branches cut and tagged: `release/app-1.0` (app 1.0) and `release/node-10.0` (pp-node 10.0) in
  pp-browser, `release/1.0` (relay 1.0) in pp-relay-apis.
- [ ] The version table lists, for each of the three, the format versions it writes and reads.
- [ ] Relay on staging, then the app lab and the mixed-version check there; app, pp-node and relay to prod together.
- [ ] `min_client_version` and the node floor raised to release 1. Prod's `client-compat` set none on 2026-11-17
  (`min_client_version` empty).
- [ ] Release candidates built with the prod origin only (no `PP_API_URL_QA`); dev builds keep both.
- [ ] The update-needed screen's text and translations (Kenneth).
- [ ] Prod's update links configured: `upgrade_url` and `upgrade_urls` per platform. On 2026-11-17 prod served the
  code's placeholder `https://example.test/releases`, so an "update needed" screen would link nowhere (relay
  settings, Leo).
- [ ] The release checklist moved into `docs/ops/RELEASE.md`, replacing § Checklist before tagging.
- [ ] The regression tiers moved into `docs/ops/TEST_STRATEGY.md`.
- [ ] The version table promoted to `docs/contracts/`; COMPATIBILITY.md's § No bridges replaced by the rule, its
  "Clean cutovers" table closed with the release-1 row; RC001–RC004 and RC006 marked superseded by those docs,
  RC005, RC007 and RC008 by `docs/ops/RELEASE.md`.

## P5 — Release mode in force

- [ ] The PR template's *Contracts and docs* asks for the format, class, versions, range exit and rollback.
- [ ] CI: a signed-bytes change without a version bump fails, in every repo with signed objects.
- [ ] The mixed-version lab run (oldest and newest supported app, pp-node, relay) on released artifacts.
- [ ] The first format change under the range rule, as the worked example.
- [ ] The full lab run split across lab machines, per-scenario times tracked, the flaky-scenario quarantine.
- [ ] The range exit and security release runbooks, before the first window closes; key rotation, the pp-node fleet
  rollback and environment resets after.
- [ ] A separate dev app ("PP Dev", its own bundle id) for mixed-version testing on one phone.
- [ ] Each [DESIGN § Not yet designed](DESIGN.md#not-yet-designed) topic designed, or moved earlier when release 1
  needs it.
