# Releasing pp-browser and pp-node

**Tier:** ops

Two release trains share this monorepo but ship independently ([release-compatibility RC016](../../projects/release-compatibility/DECISIONS.md#rc016--app-pp-node-and-relay-each-have-their-own-version)):

| Train | Release branch | Tags | Workflow | Publishes |
|-------|----------------|------|----------|-----------|
| **App** | `release/app-X.Y` | `vX.Y.Z-rc.<n>`, `vX.Y.Z` | [`release.yml`](../../.github/workflows/release.yml) (+ [`ios-testflight.yml`](../../.github/workflows/ios-testflight.yml)) | macOS DMG, Windows NSIS, Android APK → GitHub Release; iOS → TestFlight |
| **Node** | `release/node-X.Y` | `pp-node/vX.Y.Z-rc.<n>`, `pp-node/vX.Y.Z` | [`release-pp-node.yml`](../../.github/workflows/release-pp-node.yml) | Linux tarball + GHCR image → GitHub Release `pp-node …` |

When [macOS signing secrets](MACOS_SIGNING.md#github-repository-secrets) are configured, app release CI code-signs and notarizes the macOS DMG. Until then, macOS artifacts ship unsigned (Gatekeeper override required). **Full setup guide:** [MACOS_SIGNING.md](MACOS_SIGNING.md).

The design behind this page: [release-compatibility DESIGN § Branches and tags](../../projects/release-compatibility/DESIGN.md#branches-and-tags).

Step by step, with who does what and how to check it: the [release, hotfix and app incident runbooks](runbooks/README.md).

## Branching

| Branch | Role |
|--------|------|
| **`main`** (default) | The integration line: every PR lands here ([build CI](../../.github/workflows/build.yml)). Not production: no release tag goes on main |
| **`release/app-X.Y`** | One per app release, cut from main; takes `vX.Y.*` tags |
| **`release/node-X.Y`** | One per pp-node release, cut from main; takes `pp-node/vX.Y.*` tags |

```text
main ──cut──► release/app-1.0 ──tag──► v1.0.0-rc.1 … v1.0.0-rc.<n> ──tag (same commit)──► v1.0.0
     └─cut──► release/node-10.0 ──tag──► pp-node/v10.0.0-rc.1 … ──tag (same commit)──► pp-node/v10.0.0
```

A release branch takes fixes only. Fixes land on main first and are then backported to each live release branch that needs them (DESIGN [§ Release lines are first-class](../../projects/release-compatibility/DESIGN.md#release-lines-are-first-class)); release branches are never merged back. A `backport release/app-X.Y` label on the merged main PR has the backport bot open the cherry-pick PR ([§ Release branches and backports](#release-branches-and-backports)).

The libraries (pp-cpp-*) and pp-ledger have only `main`: pins are release tags cut on their `main`.

## Tags

Release tags are immutable: never move or re-push one; a fix is a new candidate. The release workflows refuse any other tag form, and any tag whose commit is not on its release branch (same X.Y).

| Tag | Made by | On | What happens |
|-----|---------|----|--------------|
| `vX.Y.Z-rc.<n>` | you | `release/app-X.Y` | App release candidate: built once with version **X.Y.Z**, uploaded to TestFlight, GitHub prerelease |
| `vX.Y.Z` | you | the commit of a `vX.Y.Z-rc.<n>` | App release: **no build**; the GitHub release gets that candidate's files |
| `pp-node/vX.Y.Z-rc.<n>` | you | `release/node-X.Y` | pp-node release candidate: built once with version **X.Y.Z**, image `pp-node:X.Y.Z-rc.<n>`, GitHub prerelease |
| `pp-node/vX.Y.Z` | you | the commit of a `pp-node/vX.Y.Z-rc.<n>` | pp-node release: **no build**; the candidate's image gets `X.Y.Z`, `vX.Y.Z` (and `latest`), the release gets its tarball |
| `internal/<build number>` | `ios-testflight.yml` | the built commit | A dev build uploaded to TestFlight by hand (internal testers only, never promoted) |
| `rc-build/<build number>` | `ios-testflight.yml` | the candidate's commit | The TestFlight build number a release candidate was uploaded with |
| `node-internal/<YYYY-MM-DD>.<n>` | `release-pp-node.yml` | the built commit (on main) | An internal pp-node build (Run workflow); not a release |

Workflows create their tags only after the build (and the upload, or the image smoke) succeeded, and never move one: creating a tag that exists fails the job. So `TestFlight 1.0.0 (152)` is `git show internal/152` or `git show rc-build/152`.

Numbers: `-rc.<n>` counts from 1 within a version. TestFlight build numbers are one counter for dev builds and candidates: the next after the highest `internal/<n>` or `rc-build/<n>` (and after 143, the last upload before builds were tagged). The older `v1.0.0-rcN` / `pp-node/v9.0.0` forms are no longer accepted.

## Maintainer flow

### Release candidate → release (app)

1. Land work on **`main`** (PRs; build CI green).
2. Cut the release branch from main once per X.Y: `git push origin main:release/app-1.0`.
3. Tag a candidate on the branch and push it:

```bash
git fetch origin && git checkout --detach origin/release/app-1.0
git tag -a v1.0.0-rc.1 -m "pp-browser 1.0.0 rc.1"
git push origin v1.0.0-rc.1
```

4. [`release.yml`](../../.github/workflows/release.yml) builds it once, with version 1.0.0 (not `1.0.0-rc.1`: only the build number differs between candidates): macOS, Windows and Android, and the TestFlight upload through [`ios-testflight.yml`](../../.github/workflows/ios-testflight.yml) (tagging `rc-build/<n>`). When all of them pass, it publishes the GitHub prerelease `pp-browser v1.0.0-rc.1`.
5. Test the candidate. A fix lands on main, is backported to `release/app-1.0`, and gets the next candidate (`v1.0.0-rc.2`).
6. Release the candidate that passed by tagging **its commit**: `git tag -a v1.0.0 <commit of v1.0.0-rc.N> -m "pp-browser 1.0.0" && git push origin v1.0.0`. Nothing is rebuilt: the `promote` job publishes the GitHub release `pp-browser v1.0.0` with that candidate's files (same bytes, their SHA-256 listed) and names its TestFlight build. It is marked latest only when it is the newest `vX.Y.Z`.

A hotfix is the same from the affected release branch: `v1.0.1-rc.1`, then `v1.0.1` on its commit. The guard refuses `vX.Y.Z-rc.<n>` once `vX.Y.Z` exists.

### Release candidate → release (pp-node)

The same steps with `release/node-X.Y` and `pp-node/vX.Y.Z-rc.<n>` / `pp-node/vX.Y.Z`. A candidate pushes `ghcr.io/<owner>/pp-node:X.Y.Z-rc.<n>` (and `:vX.Y.Z-rc.<n>`) and runs the L0 smoke on it; the release re-tags that image (`docker buildx imagetools create`: same digest) as `:X.Y.Z`, `:vX.Y.Z` and, when it is the newest `pp-node/vX.Y.Z`, `:latest`.

The node's version comes from the tag (passed as `PP_BROWSER_RELEASE_VERSION`); the app's comes from the tag too (`-DPP_BROWSER_VERSION` / `PP_BROWSER_RELEASE_VERSION`), so `PP_BROWSER_VERSION` in [`CMakeLists.txt`](../../CMakeLists.txt) matters only to dev builds.

### Dev and internal builds

- **iOS dev build:** Actions → **ios-testflight** → Run workflow on any branch or tag. It uploads to TestFlight for internal testers and tags the commit `internal/<build number>`. Never promoted: only candidates are ([RC008](../../projects/release-compatibility/DECISIONS.md#rc008--app-builds-climb-one-ladder-only-release-candidates-are-promoted)). Runs one at a time (a newer waiting run cancels an older waiting one, a candidate's upload included: re-run it).
- **Internal pp-node build:** Actions → **release-pp-node** → Run workflow with a ref on main (refused otherwise). It builds `0.0.0-dev.<sha>`, pushes that image, runs the L0 smoke, and tags the commit `node-internal/<YYYY-MM-DD>.<n>` (UTC date, `n` the next free that day). No GitHub Release.
- **Android and desktop test builds:** there is no by-hand workflow yet. [build.yml](../../.github/workflows/build.yml) keeps a debug APK and a Linux tarball of each main push as Actions artifacts (CI builds, not tagged).

### App workflow (`v*`)

[`release.yml`](../../.github/workflows/release.yml), for a candidate:

- `guard`: the tag form, the release branch, and for a release the candidate on its commit
- macOS (our self-hosted Mac, repo variable `CI_MAC_RELEASE_LABELS`; hosted `macos-15` when unset; built for macOS 14+ — `PP_BROWSER_MACOS_DEPLOYMENT_TARGET`): `PP.app`, optional sign/notarize, `.dmg`
- Windows (`windows-2022`): NSIS `.exe`
- Android (`ubuntu-24.04`): release APK
- iOS: [`ios-testflight.yml`](../../.github/workflows/ios-testflight.yml) as a called workflow (same Mac runners): IPA uploaded to TestFlight
- `release`: the GitHub prerelease, once all of the above passed

Uses `-DPP_BROWSER_PACKAGED_BUILD=ON`. For a release (`vX.Y.Z`) only `guard` and `promote` run.

### Node workflow (`pp-node/v*`)

[`release-pp-node.yml`](../../.github/workflows/release-pp-node.yml), for a candidate or an internal build:

- `guard`: the tag form and release branch, or (Run workflow) that the ref is on main
- Ubuntu 24.04 build (`-DPP_BROWSER_HEADLESS=ON`) via [`scripts/platform/pp_node_package_linux.sh`](../../scripts/platform/pp_node_package_linux.sh)
- Push `ghcr.io/<owner>/pp-node:<tag version>` and `:v<tag version>`
- L0 HTTP smoke ([IMAGE_SMOKE.md](../../packaging/pp-node/IMAGE_SMOKE.md))
- Candidate: GitHub prerelease `pp-node X.Y.Z-rc.<n>` with the Linux tarball. Internal build: the `node-internal/…` tag

For a release (`pp-node/vX.Y.Z`) only `guard` and `promote` run.

### Coming

Not in these workflows yet (release-compatibility P2):

- **Store promotion workflow** (TODO): moves a release candidate's TestFlight build to external testers and App Store review through the App Store Connect API (and the Play Developer API), behind a GitHub environment approval; refuses anything that is not a candidate; tags `vX.Y.Z` when it submits. It needs App Store Connect and Play API keys as secrets. Until then a candidate's build is sent to external testers and review by hand in App Store Connect, and `vX.Y.Z` is tagged by hand. Android has no Play upload at all yet (the APK's `versionCode` is fixed at 1).
- GitHub environments (staging / production approval gates), phased store releases.

## Artifacts

`<version>` is X.Y.Z for a candidate and its release alike: a release's files are its candidate's.

### App (`v*`)

| Platform | File | Contents |
|----------|------|----------|
| macOS (Apple Silicon) | `pp-browser-<version>-macos.dmg` | Drag-and-drop `PP.app` |
| Windows x64 | `pp-browser-<version>-windows-x64.exe` | NSIS installer |
| Android | `pp-browser-<version>-android.apk` | Universal APK |

### Node (`pp-node/v*`)

| Artifact | Contents |
|----------|----------|
| `pp-node-<version>-linux-amd64.tar.gz` | Stripped binary + config example + systemd unit |
| `ghcr.io/people-post/pp-node:<tag version>` | Same binary on `ubuntu:24.04` (`X.Y.Z-rc.<n>` for a candidate; its release adds `X.Y.Z`) |

```bash
docker pull ghcr.io/people-post/pp-node:0.1.0

docker run --rm -it \
  --cap-add=NET_BIND_SERVICE \
  -e PP_BROWSER_PIN=... \
  -e PP_NODE_AMP_UDP_PORT=443 \
  -e PP_NODE_DATA_DIR=/var/lib/pp-node \
  -e PP_NODE_STATUS_ADDR=0.0.0.0:18518 \
  -v pp-node-data:/var/lib/pp-node \
  -p 443:443/udp \
  -p 18518:18518 \
  ghcr.io/people-post/pp-node:0.1.0
```

Deploy overlays: [CONFIGURATION.md](CONFIGURATION.md#pp-node-deploy-overlays). Local compose / L0–L1 smoke: [IMAGE_SMOKE.md](../../packaging/pp-node/IMAGE_SMOKE.md), [BUILD.md](BUILD.md#headless-mesh-node-pp-node).

## macOS code signing and notarization

See **[MACOS_SIGNING.md](MACOS_SIGNING.md)**.

## Installing unsigned builds

### macOS

1. Open the `.dmg` and drag **PP** to Applications.
2. First launch may need Right-click → **Open**, or allow under **Privacy & Security**.

### Windows

SmartScreen may warn; **More info** → **Run anyway**, or Unblock the file.

## Local packaging smoke

**App** (macOS / Windows):

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DPP_BROWSER_PACKAGED_BUILD=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build --prefix install
cpack --config build/CPackConfig.cmake
```

**Node** (Ubuntu 24.04 host):

```bash
PP_BROWSER_RELEASE_VERSION=0.0.0-local bash scripts/platform/pp_node_package_linux.sh all
./scripts/test/pp_local_test.sh run --suite node
# or: docker compose -f packaging/pp-node/docker-compose.yml up -d
#     ./scripts/test/pp_node_relay_smoke.sh
```

## Checklist before tagging

### App candidate (`vX.Y.Z-rc.<n>`)

- [ ] Fixes on **`main`** first, backported to **`release/app-X.Y`**
- [ ] The commit is on `release/app-X.Y` (the guard refuses it otherwise)
- [ ] `release/app-X.Y` CI green
- [ ] iOS signing / App Store Connect secrets configured (the candidate fails without its TestFlight upload); macOS signing secrets (optional)

### App release (`vX.Y.Z`)

- [ ] The candidate that passed is tagged on **this commit** and its GitHub prerelease exists

### Node candidate (`pp-node/vX.Y.Z-rc.<n>`)

- [ ] The commit is on **`release/node-X.Y`**
- [ ] Node-relevant change since the last tag: `./scripts/dev/pp_node_closure.sh --build <dir>` (build-derived: closure files changed since the newest `pp-node/v*` tag + sibling pin bumps; info only)
- [ ] Local L0 (and L1/L2 N-FANOUT if probe built) green — [IMAGE_SMOKE.md](../../packaging/pp-node/IMAGE_SMOKE.md)
- [ ] GHCR package visibility set if public pulls are required
- [ ] Protocol/compat note if this release breaks older apps

### Node release (`pp-node/vX.Y.Z`)

- [ ] The candidate that passed is tagged on **this commit** and its GitHub prerelease exists

## Future: Windows code signing

- Import Authenticode `.pfx` as a secret; sign NSIS / exe with `signtool`

## Release branches and backports

Each product's release that ships into the field gets a branch cut from `main`, `release/app-X.Y` or `release/node-X.Y`, and it stays open while that release is in the supported range ([release-compatibility DESIGN § Release lines](../../projects/release-compatibility/DESIGN.md#release-lines-are-first-class), [RC014](../../projects/release-compatibility/DECISIONS.md#rc014--backport-and-hotfix-rules-after-leos-second-review), [RC016](../../projects/release-compatibility/DECISIONS.md#rc016--app-pp-node-and-relay-each-have-their-own-version)).

**What a release branch takes:** fixes only. No features and no format changes; an additive change only where a fix needs it; a security cutover may go on. The newest branch takes every fix; older branches still in range take only security fixes and fixes for a break between in-range versions. Release branches are never merged back into `main`.

**Cutting one** (the release owner): `git push origin main:refs/heads/release/app-1.0`. [`release-issue.yml`](../../.github/workflows/release-issue.yml) then opens the issue "Release app 1.0" from [`.github/ISSUE_TEMPLATE/release.md`](../../.github/ISSUE_TEMPLATE/release.md), assigned to whoever pushed the branch, where the checklist is ticked and the go / no-go recorded, and creates the label `backport release/app-1.0`. [Build CI](../../.github/workflows/build.yml) runs on the branch's pushes and on PRs to it, with every platform and every test (not a PR's subset).

**Fixes land on `main` first, then are backported:**

1. Merge the fix to `main` as usual.
2. Label the PR `backport release/app-1.0`, before or after the merge; one label per branch that needs it (a fix in code app and pp-node share takes one per product's branch).
3. [`backport.yml`](../../.github/workflows/backport.yml) cherry-picks the PR's commits with `-x` (each keeps its `(cherry picked from commit …)` trailer) onto `backport/pr-<N>-to-release-app-1.0`, cut from the target, and opens a PR to `release/app-1.0` that links the original. It asks the release owner (repo variable `RELEASE_OWNER`; the original author when unset or when the request fails) for review, and comments the link on the original. The commits: a merge commit's PR commits in order (merges inside the PR skipped), a squash merge's one commit, or a rebase merge's commits. Commits already on the target are skipped.
4. The backport PR runs that branch's checks and needs a human approval (the release owner's may do).

**Checks on the bot's PRs.** A PR opened, or a branch pushed, with the workflow token (`GITHUB_TOKEN`, the only token the bot has) starts no `pull_request` or `push` run, so the bot dispatches `build.yml` on its branch; those check runs land on the PR's head commit. A human push to the branch then runs the normal PR checks. If a run is missing: Actions → build → Run workflow on the backport branch, or close and reopen the PR.

**When it does not apply:** the bot pushes nothing and comments on the original PR with the conflicting commit, its files, and the commands to do the backport by hand. It never resolves a conflict. Whoever does it says what differs on a line `branch-specific backport: …` in the commit message, in a separate commit where the rest picks cleanly. The bot also refuses, with a comment, a label naming anything but an existing `release/app-X.Y` or `release/node-X.Y`, and a backport touching `.github/workflows/` (the workflow token cannot push it): those are done by hand the same way.

**A fix only a release branch needs** (the code is gone from `main`, say) is the exception: a PR straight to the branch with the label `release-branch-only` and a line `Why not on main: …` in its body. [`release-branch-pr.yml`](../../.github/workflows/release-branch-pr.yml) checks every PR to a release branch: either that label and reason, or every commit carries a cherry-pick trailer naming a commit on `main` or a `branch-specific backport` line.

**A hotfix starts from the affected branch**, not only the newest: backport to `release/app-1.1` (or the exception above), and tag `v1.1.Z+1` there, its candidates `-rc.N` first.

**Closing a branch** (the release owner, when the version table says its release has left the range): close its open backport PRs with a note naming the range exit (`gh pr list --base release/app-1.0 --state open --json number --jq '.[].number' | xargs -I{} gh pr close {} --comment "release/app-1.0 left the supported range on <date>; not backported."`); rename the label `backport release/app-1.0` to `closed release/app-1.0`, so the bot no longer takes it and the PRs keep it as a record; then lock the branch against pushes in branch protection. The branch is kept: its tags and history must stay.

## Deferred

| Item | Notes |
|------|-------|
| Intel macOS / universal binary | GHA `macos-15` is arm64 only |
| Linux `.deb` / AppImage (GUI) | Not targeted |
| `pp-node` multi-arch (`linux/arm64`) | amd64 only for now |
| L1 / L2 N-FANOUT / N-CAP in release CI | Local scripts done; CI optional — [IMAGE_SMOKE.md](../../packaging/pp-node/IMAGE_SMOKE.md) |
| Auto-update channel | Separate effort |
| iOS IPA on the GitHub Release | Candidates go to TestFlight ([IOS_BUILD.md](IOS_BUILD.md)); the IPA is a 14-day Actions artifact only |
