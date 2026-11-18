# Release compatibility: phases

Order of work. Status is in [CURRENT_STATE.md](CURRENT_STATE.md).

## P0 — Inventory

- [ ] Every format in [DESIGN § Scope](DESIGN.md#scope), per repo (pp-browser, pp-cpp-amp, pp-cpp-mesh, pp-cpp-common,
  pp-cpp-crypto, pp-relay-apis): its version, where the receiver checks it, what an unknown version or field does
  today.
- [ ] Each checked against [DESIGN § Release 1](DESIGN.md#release-1); the result is the list of release-1 work items,
  needed and worth-doing marked apart.
- [ ] The breaking work in flight: which lands before the freeze, which waits for the range rule.
- [ ] Leo's review of the relay's parts ([DESIGN § Decisions taken](DESIGN.md#decisions-taken), items 3 and 8).

## P1 — Freeze

- [ ] No new breaking work outside the P0 list until release 1 ships.

## P2 — One main, release branches

[DESIGN § Branches and tags](DESIGN.md#branches-and-tags), in every repo, before release 1 is cut:

- [ ] develop merged into main a last time; open PRs retargeted to main; develop deleted (pp-browser, pp-cpp-*,
  pp-ledger; pp-relay-apis with Leo).
- [ ] Every internal build tagged by its workflow: TestFlight, Android and desktop test builds, release candidates,
  internal pp-node builds, the relay's dev deploys.
- [ ] Release workflows take release tags only from `release/*` branches, build once, and promote the artifact.
- [ ] GitHub environments: main → dev; release candidates → staging; only `v*` tags → production, behind an approval.
- [ ] The backport bot (a label opens the backport PR, the cherry-pick trailer kept).
- [ ] App builds ([DESIGN § App builds](DESIGN.md#app-builds-internal-external-store)): release-candidate tags build
  and upload automatically; the promotion workflow (App Store Connect and Play APIs, approval-gated, refuses non-candidates,
  tags `vX.Y.Z`); phased store releases; the app shows its tag and commit in About and diagnostics.
- [ ] The check levels in CI ([DESIGN § Checks by level](DESIGN.md#checks-by-level)).
- [ ] Environments ([DESIGN § Environments](DESIGN.md#environments), Leo's infrastructure): dev created (relay,
  its PeerId, secrets, tables); qa becomes staging (takes release candidates only; Me → Network shows "Staging", the
  config value stays `qa`); prod canary deploys with alarms and automatic rollback; pp-node dev and staging tiers;
  a rollback rehearsal on staging.
- [ ] Backups ([DESIGN § Backups and restore](DESIGN.md#backups-and-restore), Leo's infrastructure): point-in-time
  recovery on every state table; backups before each prod release and rollback in the workflows; the locked vault
  copy in another account; the identity key and Support seed replicated and escrowed; the removal log; the restore
  runbook and a first restore rehearsal on staging.
- [ ] The PR template and `docs/ops/RELEASE.md` say what a release branch takes, that fixes land on main first, and
  how a backport is asked for.

## P3 — Release 1 built

- [ ] Each P0 item, repo by repo; the relay first (dev, then staging), as for every relay change.
- [ ] Frozen data of each release-1 format checked in, read by unit tests in each repo.
- [ ] RC011's release-1 items ([DESIGN § Release 1](DESIGN.md#release-1)): version counts at the relay; remote
  switches in `client-compat` and the app's reader; the app ↔ relay behaviour suite in both CIs; relay identity
  rotation; in-app account deletion; a crash-report destination.

## P4 — Release 1 shipped

- [ ] `release/10.0` cut in pp-browser (app and pp-node 10.0) and the relay's release branch in pp-relay-apis; release 1 tagged there.
- [ ] Relay on staging, then the app lab and the mixed-version check there; app, pp-node and relay to prod together.
- [ ] `min_client_version` and the node floor raised to release 1. Prod's `client-compat` set none on 2026-11-17
  (`min_client_version` empty).
- [ ] Prod's update links configured: `upgrade_url` and `upgrade_urls` per platform. On 2026-11-17 prod served the
  code's placeholder `https://example.test/releases`, so an "update needed" screen would link nowhere (relay
  settings, Leo).
- [ ] The version table promoted to `docs/contracts/`; COMPATIBILITY.md's § No bridges replaced by the rule, its
  "Clean cutovers" table closed with the release-1 row; RC001–RC004 and RC006 marked superseded by those docs,
  RC005, RC007 and RC008 by `docs/ops/RELEASE.md`.

## P5 — Release mode in force

- [ ] The PR template's *Contracts and docs* asks for the format, class, versions, range exit and rollback.
- [ ] CI: a signed-bytes change without a version bump fails, in every repo with signed objects.
- [ ] The mixed-version lab run (oldest and newest supported app, pp-node, relay) on released artifacts.
- [ ] The first format change under the range rule, as the worked example.
- [ ] A separate dev app ("PP Dev", its own bundle id) for mixed-version testing on one phone.
- [ ] Each [DESIGN § Not yet designed](DESIGN.md#not-yet-designed) topic designed, or moved earlier when release 1
  needs it.
