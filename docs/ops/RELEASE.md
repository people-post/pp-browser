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
| **`develop`** (default) | Integration / tip — day-to-day PRs and [build CI](../.github/workflows/build.yml) |
| **`main`** | Release line — stabilize, hotfixes, **cut tags here** |

```text
develop  ──merge──►  main  ──tag──►  v*  and/or  pp-node/v*
                ▲
                └── hotfix on main, then backport to develop
```

Do not add a third long-lived branch unless you later need N-1 maintenance lines.

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

Independent semver from the app. Breaking mesh/wire protocols still need a coordinated bump (or dual-protocol support) — see [COMPATIBILITY.md](../contracts/COMPATIBILITY.md) and protocol IDs under `src/base/mesh/`.

## Maintainer flow

### Ship from develop → main

1. Land work on **`develop`** (PRs; build CI green).
2. Open PR **`develop` → `main`** when ready to stabilize; merge.
3. On **`main`**, bump versions if needed:
   - App: `PP_BROWSER_VERSION` in [`CMakeLists.txt`](../../CMakeLists.txt)
   - Node: version comes from the **`pp-node/v…` tag** (passed as `PP_BROWSER_RELEASE_VERSION` at build time)
4. Tag **from `main`** and push (one or both trains):

```bash
# App only
git checkout main && git pull
git tag -a v0.1.0 -m "pp-browser 0.1.0"
git push origin v0.1.0

# Node only
git tag -a pp-node/v0.1.0 -m "pp-node 0.1.0"
git push origin pp-node/v0.1.0
```

5. Hotfix: commit on **`main`**, tag again, then merge/cherry-pick back to **`develop`**.

CI refuses tags whose commit is not on `origin/main`.

### App workflow (`v*`)

[`release.yml`](../.github/workflows/release.yml):

- macOS (`macos-14`): `PP.app`, optional sign/notarize, `.dmg`
- Windows (`windows-2022`): NSIS `.exe`
- Android (`ubuntu-24.04`): release APK

Uses `-DPP_BROWSER_PACKAGED_BUILD=ON`.

### Node workflow (`pp-node/v*`)

[`release-pp-node.yml`](../.github/workflows/release-pp-node.yml):

- Ubuntu 24.04 build (`-DPP_BROWSER_HEADLESS=ON`) via [`scripts/pp_node_package_linux.sh`](../../scripts/pp_node_package_linux.sh)
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
PP_BROWSER_RELEASE_VERSION=0.0.0-local bash scripts/pp_node_package_linux.sh all
./scripts/pp_local_test.sh run --suite node
# or: docker compose -f packaging/pp-node/docker-compose.yml up -d
#     ./scripts/pp_node_relay_smoke.sh
```

## Checklist before tagging

### App (`v*`)

- [ ] Changes merged to **`main`**
- [ ] Version bumped in `CMakeLists.txt` when needed
- [ ] `develop` / `main` build CI green
- [ ] Smoke-tested packaged build locally if possible
- [ ] macOS signing secrets configured (optional)

### Node (`pp-node/v*`)

- [ ] Changes merged to **`main`**
- [ ] Local L0 (and L1/L2 N-FANOUT if probe built) green — [IMAGE_SMOKE.md](../../packaging/pp-node/IMAGE_SMOKE.md)
- [ ] GHCR package visibility set if public pulls are required
- [ ] Protocol/compat note if this release breaks older apps

## Future: Windows code signing

- Import Authenticode `.pfx` as a secret; sign NSIS / exe with `signtool`

## Deferred

| Item | Notes |
|------|-------|
| Intel macOS / universal binary | GHA `macos-14` is arm64 only |
| Linux `.deb` / AppImage (GUI) | Not targeted |
| `pp-node` multi-arch (`linux/arm64`) | amd64 only for now |
| L1 / L2 N-FANOUT / N-CAP in release CI | Local scripts done; CI optional — [IMAGE_SMOKE.md](../../packaging/pp-node/IMAGE_SMOKE.md) |
| Auto-update channel | Separate effort |
| iOS distribution | [PLATFORMS.md](../architecture/PLATFORMS.md) |
