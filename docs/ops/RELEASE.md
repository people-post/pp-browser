# Releasing pp-browser and pp-node

**Tier:** ops

Two release trains share this monorepo but ship independently:

| Train | Tag | Workflow | Publishes |
|-------|-----|----------|-----------|
| **App** | `v1.2.3` | [`release.yml`](../.github/workflows/release.yml) | macOS DMG, Windows NSIS, Android APK → GitHub Release |
| **Node** | `pp-node/v0.4.0` | [`release-pp-node.yml`](../.github/workflows/release-pp-node.yml) | Linux tarball + GHCR image → GitHub Release `pp-node …` |

When [macOS signing secrets](MACOS_SIGNING.md#github-repository-secrets) are configured, app release CI code-signs and notarizes the macOS DMG. Until then, macOS artifacts ship unsigned (Gatekeeper override required). **Full setup guide:** [MACOS_SIGNING.md](MACOS_SIGNING.md).

## Branching

| Branch | Role |
|--------|------|
| **`main`** (default) | The integration line — day-to-day PRs, [build CI](../.github/workflows/build.yml) on PRs and pushes, **cut tags here** |

```text
main  ──tag──►  v*  and/or  pp-node/v*
```

There is no `develop`. Until the release branches of [release-compatibility DESIGN § Branches and tags](../../projects/release-compatibility/DESIGN.md#branches-and-tags) exist (later in its P2), every release is a tag cut from `main`.

The libraries (pp-cpp-*) and pp-ledger also have only `main`: pins are release tags cut on their `main`.

## Tag conventions

### App (`v*`)

| Tag | Meaning |
|-----|---------|
| `v0.1.0` | Stable app release |
| `v0.2.0-rc1` | Pre-release (GitHub prerelease) |

Pattern: `vMAJOR.MINOR.PATCH` with optional suffixes (`-rc1`, `-beta1`, …).

### Node (`pp-node/v*`)

| Tag | Meaning |
|-----|---------|
| `pp-node/v0.1.0` | Stable node / GHCR release |
| `pp-node/v0.1.0-rc1` | Pre-release (no `:latest` image tag) |

Independent semver from the app. Breaking mesh/wire protocols still need a coordinated bump (or dual-protocol support) — see [COMPATIBILITY.md](../contracts/COMPATIBILITY.md) and protocol IDs in pp-cpp-mesh `mesh/l4/shared/L4ProtocolIds.h`.

## Maintainer flow

### Ship from main

1. Land work on **`main`** (PRs; build CI green).
2. On **`main`**, bump versions if needed:
   - App: `PP_BROWSER_VERSION` in [`CMakeLists.txt`](../../CMakeLists.txt)
   - Node: version comes from the **`pp-node/v…` tag** (passed as `PP_BROWSER_RELEASE_VERSION` at build time)
3. Tag **from `main`** and push (one or both trains):

```bash
# App only
git checkout main && git pull
git tag -a v0.1.0 -m "pp-browser 0.1.0"
git push origin v0.1.0

# Node only
git tag -a pp-node/v0.1.0 -m "pp-node 0.1.0"
git push origin pp-node/v0.1.0
```

4. Fix: land it on **`main`**, tag again.

CI refuses tags whose commit is not on `origin/main`.

### App workflow (`v*`)

[`release.yml`](../.github/workflows/release.yml):

- macOS (our self-hosted Mac, repo variable `CI_MAC_RELEASE_LABELS`; hosted `macos-15` when unset; built for macOS 14+ — `PP_BROWSER_MACOS_DEPLOYMENT_TARGET`): `PP.app`, optional sign/notarize, `.dmg`
- Windows (`windows-2022`): NSIS `.exe`
- Android (`ubuntu-24.04`): release APK

Uses `-DPP_BROWSER_PACKAGED_BUILD=ON`.

### Node workflow (`pp-node/v*`)

[`release-pp-node.yml`](../.github/workflows/release-pp-node.yml):

- Ubuntu 24.04 build (`-DPP_BROWSER_HEADLESS=ON`) via [`scripts/platform/pp_node_package_linux.sh`](../../scripts/platform/pp_node_package_linux.sh)
- Push `ghcr.io/<owner>/pp-node:<version>` (and `:v…`, `:latest` when not a prerelease)
- L0 HTTP smoke ([IMAGE_SMOKE.md](../../packaging/pp-node/IMAGE_SMOKE.md))
- GitHub Release named `pp-node <version>` with the Linux tarball

Optional dogfood: Actions → **release-pp-node** → **Run workflow** (builds a `0.0.0-dev.<sha>` image; no GitHub Release).

## Artifacts

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
| `ghcr.io/people-post/pp-node:<version>` | Same binary on `ubuntu:24.04` |

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

### App (`v*`)

- [ ] Changes merged to **`main`**
- [ ] Version bumped in `CMakeLists.txt` when needed
- [ ] `main` build CI green
- [ ] Smoke-tested packaged build locally if possible
- [ ] macOS signing secrets configured (optional)

### Node (`pp-node/v*`)

- [ ] Changes merged to **`main`**
- [ ] Node-relevant change since the last tag: `./scripts/dev/pp_node_closure.sh --build <dir>` (build-derived: closure files changed since the newest `pp-node/v*` tag + sibling pin bumps; info only)
- [ ] Local L0 (and L1/L2 N-FANOUT if probe built) green — [IMAGE_SMOKE.md](../../packaging/pp-node/IMAGE_SMOKE.md)
- [ ] GHCR package visibility set if public pulls are required
- [ ] Protocol/compat note if this release breaks older apps

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
| iOS on `v*` release tags | Manual TestFlight: [`ios-testflight.yml`](../.github/workflows/ios-testflight.yml) — [IOS_BUILD.md](IOS_BUILD.md); fold into `release.yml` later |
