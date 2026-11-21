# Runbook: release

**Tier:** ops

Shipping a new app or pp-node release, from the cut to the store. The checklist is the release issue
([template](../../../.github/ISSUE_TEMPLATE/release.md)); this page says how each item is done. Examples use app 1.0
and pp-node 10.0; tag forms and workflows: [RELEASE.md](../RELEASE.md).

- **When:** every new X.Y of the app or pp-node. A fix to a release already cut is a [hotfix](hotfix.md).
- **Who:** the release owner ([README § Who](README.md#who)) does every step unless one names Hongwei (relay prod
  tags) or Leo (relay settings).
- **Record:** the release issue; link each workflow run there.

## 1. Before the cut

| Step | Who | How | Verify |
|------|-----|-----|--------|
| Format changes since the last release listed, each with its class; the version table updated | release owner | `git log --oneline <last release tag>..origin/main`; the table: [DESIGN § The version table](../../../projects/release-compatibility/DESIGN.md#the-version-table) | Listed in the release issue |
| A relay release this one needs, deployed to prod first | Hongwei | In the relay API repository: `git fetch origin && git tag -a vX.Y.Z <commit on release/X.Y> -m "relay X.Y.Z" && git push origin vX.Y.Z`. Its CI's `deploy` job deploys prod (refused if the commit is not on `release/X.Y` or the pusher is not a listed deployer) | `gh run list -R <relay API repository> --workflow ci.yml --branch vX.Y.Z --limit 1`, then `gh run watch <run id> -R <relay API repository> --exit-status`: `deploy` ran, not skipped |
| Freeze announced; release owner named | release owner | The team channel | — |

A relay release candidate is held on staging before prod: tag `release/vX.Y.Z-rc.<n>` on `release/X.Y` (a dev tag `dev/<date>.<n>` on a commit of main deploys the dev environment); the relay repositories deploy only from tags ([RC021](../../../projects/release-compatibility/DECISIONS.md#rc021--the-relay-repos-deploy-by-tag-only)).

## 2. Cut the branch

The release owner pushes it (the release issue is assigned to whoever pushes), from the main commit chosen:

```bash
git fetch origin
git push origin <commit on main>:refs/heads/release/app-1.0     # pp-node: release/node-10.0
```

[`release-issue.yml`](../../../.github/workflows/release-issue.yml) opens the issue "Release app 1.0" and creates the
label `backport release/app-1.0`. Verify:

```bash
gh run list --workflow release-issue.yml --limit 1
gh issue list --label release --search '"Release app 1.0" in:title'
gh label list --search 'backport release/app-1.0'
```

[`build.yml`](../../../.github/workflows/build.yml) runs on the branch: every platform, every test. From now on the
branch takes fixes only, backported from main ([hotfix.md](hotfix.md)). Not yet: branch protection on release
branches (PHASES § P2).

## 3. Release candidates

Tag a candidate on the branch's tip and push it:

```bash
git fetch origin && git checkout --detach origin/release/app-1.0
git tag -a v1.0.0-rc.1 -m "pp-browser 1.0.0 rc.1"          # pp-node: pp-node/v10.0.0-rc.1, "pp-node 10.0.0 rc.1"
git push origin v1.0.0-rc.1
gh run list --workflow release.yml --branch v1.0.0-rc.1 --limit 1      # pp-node: release-pp-node.yml
gh run watch <run id> --exit-status
```

**What the run does.** App ([`release.yml`](../../../.github/workflows/release.yml)): `guard` checks the tag form and
that the commit is on `release/app-1.0`; then macOS DMG, Windows installer, Android APK, and the TestFlight upload
([`ios-testflight.yml`](../../../.github/workflows/ios-testflight.yml)), all built once with version 1.0.0; when all
pass, the GitHub prerelease `pp-browser v1.0.0-rc.1`. pp-node
([`release-pp-node.yml`](../../../.github/workflows/release-pp-node.yml)): the image
`ghcr.io/people-post/pp-node:10.0.0-rc.1`, the L0 smoke on it, then the prerelease with the tarball.

**Check it:**

| What | Command |
|------|---------|
| The prerelease and its files | `gh release view v1.0.0-rc.1` (DMG, EXE, APK); pp-node: `gh release view pp-node/v10.0.0-rc.1` |
| The TestFlight build number | `git fetch --tags origin && git tag --points-at 'v1.0.0-rc.1^{commit}' --list 'rc-build/*'` gives `rc-build/<n>`: TestFlight shows `1.0.0 (<n>)` (several when candidates share a commit: the highest is the newest) |
| The pp-node image | `docker buildx imagetools inspect ghcr.io/people-post/pp-node:10.0.0-rc.1` |
| The files, locally | `gh release download v1.0.0-rc.1 --dir rc1`; the IPA is the run's artifact `pp-browser-ios-1.0.0-<n>` (14 days): `gh run download <run id>` |

Then in App Store Connect, once the build is processed: answer export compliance if asked
([APP_STORE_EXPORT_COMPLIANCE.md](../APP_STORE_EXPORT_COMPLIANCE.md)) and enable `1.0.0 (<n>)` under TestFlight →
Internal Testing, with its "What to Test".

**A failed run.** A tag is never moved. If only the iOS upload was cancelled (`ios-testflight` runs one at a time; a
newer waiting run cancels an older waiting one), `gh run rerun <run id> --failed`. Anything else: fix it on main,
backport, and tag `v1.0.0-rc.2`.

**Test the candidate** and tick the issue's *Release candidate* items, linking runs:

- Real devices from internal testing: register, message, call, push and VoIP wake, link a device, account deletion.
- The lab scenarios the release touches, locally:
  `PP_APP_LAB_ARGS="--only <scenario>" ./scripts/test/pp_local_test.sh run --suite app`.
- Release notes and the TestFlight "What to Test" written.

Mark these items "not yet" in the issue instead of ticking them: the frozen-data tests (PHASES § P3); the app ↔ relay
behaviour suite (INVENTORY O3); staging and the store app (N−1) against the relay candidate there (PHASES § P2,
Environments); the nightly lab (idle until its repo variables are set, PHASES § P2, Regression suites).

## 4. Go / no-go

The release owner reviews the issue's *Go / no-go* items and writes the decision on its **Go / no-go** line (go or
no-go, the date, who, why):

- No open blocker; known issues listed in the issue.
- Rollback path written: the relay's previous tag (`git -C <pp-relay-apis> tag --list 'v*' --sort=-v:refname | head -2`).
- Not yet: version counts (INVENTORY O1); remote switches to turn this release's features off (INVENTORY O2); a relay
  backup taken by the release workflow (PHASES § P2, Backups): ask Leo.

## 5. The final tag

Tag the commit of the candidate that passed. Nothing is rebuilt: the `promote` job publishes that candidate's files.

```bash
commit=$(git rev-parse 'v1.0.0-rc.3^{commit}')
git tag -a v1.0.0 "$commit" -m "pp-browser 1.0.0"          # pp-node: pp-node/v10.0.0 on pp-node/v10.0.0-rc.<n>'s commit
git push origin v1.0.0
gh run list --workflow release.yml --branch v1.0.0 --limit 1
gh run watch <run id> --exit-status
```

Verify: `gh release view v1.0.0` lists the candidate's files with their SHA-256 and names the TestFlight build; it is
marked latest only when it is the newest `vX.Y.Z`. pp-node: `docker buildx imagetools inspect
ghcr.io/people-post/pp-node:10.0.0` shows the candidate's digest. The guard refuses a `vX.Y.Z` with no candidate
GitHub release on its commit.

Tag `v1.0.0` when the build is submitted to the store (step 6): the tag publishes the desktop and Android files on
GitHub at once. The store binary is the candidate's, so About in it shows the candidate tag (`Build v1.0.0-rc.3 · …`,
[BUILD.md § Build identity](../BUILD.md#build-identity-commit-and-tag)).

## 6. Store submission

By hand in App Store Connect today. Not yet: the promotion workflow that moves a candidate to external testers and
review through the App Store Connect and Play APIs and tags `vX.Y.Z` itself
([RELEASE.md § Coming](../RELEASE.md#coming), PHASES § P2).

1. **External testers:** TestFlight → the external group → add build `1.0.0 (<n>)`. The first build of a version waits
   for Beta App Review (usually under a day). External testers run it against the prod relay.
2. **Review:** App Store → the 1.0.0 version → Build: select `1.0.0 (<n>)` (the `rc-build/<n>` of the candidate that
   passed, nothing else) → Version Release: phased release on (step 7) → Add for Review → submit. Tag `v1.0.0` now
   (step 5).
3. **Android:** not yet: a Play upload (the APK's `versionCode` is fixed at 1); the APK is on the GitHub release.
4. **Desktop:** the GitHub release `pp-browser v1.0.0` is the download.

Verify: App Store Connect shows the version "Waiting for Review" with build `<n>`.

## 7. Phased release

Every update goes out over the App Store's 7-day phased release, so a bad version stops at a few percent
([app-incident.md](app-incident.md) pauses it). The first public version of the app has no phased release (it
applies to updates). Users can still update by hand during it.

- Watch: crash reports for the build in App Store Connect (TestFlight → Crashes, and Xcode Organizer), the relay's
  alarms (Leo), user reports.
- Not yet: Play staged rollout (no Play upload); a crash-report destination of ours (INVENTORY O6).
- pp-node: Leo rolls the fleet out (ansible); owners of user-run nodes are told the image tag.

## 8. After the release

| Step | Who | How | Verify |
|------|-----|-----|--------|
| `latest_client_version` raised to the release (and `min_client_version` when the release raises the floor: `1.0.0` at release 1); `upgrade_url` / `upgrade_urls` per platform | Leo (relay settings), asked by the release owner | — | `curl -s https://api.pp.peoplepost.org/api/relay/v1/client-compat \| jq` ([SERVICE_ENDPOINTS § Client compatibility](../../contracts/SERVICE_ENDPOINTS.md#client-compatibility-discovery)) |
| The version table: this release's format versions; the branch's open period | release owner | A PR to [DESIGN § The version table](../../../projects/release-compatibility/DESIGN.md#the-version-table) (it moves to `docs/contracts/` at release 1, PHASES § P4) | Merged |
| The release issue closed | release owner | `gh issue close <n> --comment "Released <date>: <links>"` | — |
| Anything went wrong | release owner | An `incident` issue and a postmortem ([app-incident.md § 7](app-incident.md#7-follow-up)) | — |
