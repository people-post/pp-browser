# Releasing pp-browser

**Tier:** ops

Tag-triggered CI builds macOS and Windows installers, an Android release APK, a Linux **`pp-node`** tarball, and publishes them to [GitHub Releases](https://github.com/people-post/pp-browser/releases). The same workflow pushes a **`pp-node`** container image to GHCR.

When [macOS signing secrets](MACOS_SIGNING.md#github-repository-secrets) are configured, release CI code-signs and notarizes the macOS DMG. Until then, macOS artifacts ship unsigned (Gatekeeper override required). **Full setup guide:** [MACOS_SIGNING.md](MACOS_SIGNING.md).

## Tag convention

Use semver tags with a `v` prefix:

| Tag | Meaning |
|-----|---------|
| `v0.1.0` | Stable release |
| `v0.2.0-rc1` | Pre-release (marked as prerelease on GitHub) |
| `v0.2.0-beta1` | Pre-release |

The workflow matches tags of the form `vMAJOR.MINOR.PATCH` with optional suffixes (`-rc1`, `-beta1`, etc.).

## Maintainer flow

1. Bump the version in [`CMakeLists.txt`](../CMakeLists.txt) (`PP_BROWSER_VERSION` default / `project(VERSION ...)`) on `main`.
2. Commit and push.
3. Create and push an annotated tag:

```bash
git tag -a v0.1.0 -m "pp-browser 0.1.0"
git push origin v0.1.0
```

4. GitHub Actions workflow [`.github/workflows/release.yml`](../.github/workflows/release.yml) runs automatically:
   - **macOS** (`macos-14`): builds `Frame.app`, optionally signs + notarizes, packages a `.dmg`
   - **Windows** (`windows-2022`): builds the app, packages an NSIS `.exe` installer
   - **Android** (`ubuntu-24.04`, NDK `27.0.12077973`): builds a release APK (`assembleRelease`) with native code compiled in Release mode
   - **Linux pp-node** (`ubuntu-24.04`): stripped headless binary, tarball on the GitHub Release, image pushed to GHCR (`ubuntu:24.04`)
5. When all jobs succeed, a GitHub Release is created with the artifacts attached.

Release CI uses the same OS runners, Android NDK, and compiler-cache setup as [build CI](../.github/workflows/build.yml) for GUI/mobile targets. Linux **desktop** packages are still not published; only **`pp-node`** is.

Release builds use:

- `-DPP_BROWSER_PACKAGED_BUILD=ON` — runtime asset paths relative to the installed executable / bundle (GUI installers)
- `pp-node`: `-DPP_BROWSER_HEADLESS=ON` on Ubuntu 24.04 (no X11/GL); see [`scripts/pp_node_package_linux.sh`](../../scripts/pp_node_package_linux.sh)

## Artifacts

| Platform | File / image | Contents |
|----------|--------------|----------|
| macOS (Apple Silicon) | `pp-browser-<version>-macos.dmg` | Drag-and-drop install of `Frame.app` (DMG volume name Frame) |
| Windows x64 | `pp-browser-<version>-windows-x64.exe` | NSIS installer (display name Frame; exe + `assets/` under install dir) |
| Android | `pp-browser-<version>-android.apk` | Universal APK (`armeabi-v7a`, `arm64-v8a`, `x86_64`); signed with the debug keystore until a release keystore is configured |
| Linux (amd64) | `pp-node-<version>-linux-amd64.tar.gz` | Stripped `pp-node` + config example + systemd unit (Ubuntu 24.04 glibc) |
| GHCR | `ghcr.io/people-post/pp-node:<version>` | Same binary on `ubuntu:24.04`. Also tagged `:v…` and `:latest` on non-prerelease tags |

### `pp-node` glibc / image contract

Build and runtime stay on the **same OS family**: Ubuntu 24.04. Release CI compiles on the `ubuntu-24.04` runner; the Dockerfile is [`packaging/pp-node/Dockerfile`](../../packaging/pp-node/Dockerfile) (`FROM ubuntu:24.04`). Prefer this match over a smaller unrelated base (Debian bookworm, Alpine, distroless).

```bash
# Pull (package may be private until made public in GHCR settings)
docker pull ghcr.io/people-post/pp-node:0.1.0

docker run --rm -it \
  --cap-add=NET_BIND_SERVICE \
  -e PP_BROWSER_PIN=... \
  -v pp-node-data:/var/lib/pp-node \
  -p 443:443 \
  ghcr.io/people-post/pp-node:0.1.0
```

Status HTTP stays on loopback inside the container (`127.0.0.1:18518`); use `docker exec` / `kubectl exec` to query it. See [BUILD.md](BUILD.md#headless-mesh-node-pp-node).

## macOS code signing and notarization

See **[MACOS_SIGNING.md](MACOS_SIGNING.md)** for the full guide: Apple Developer Portal setup, GitHub secrets, local smoke test, CI flow, and troubleshooting.

## Installing unsigned builds

When signing secrets are **not** configured, macOS artifacts are unsigned. Users may need to override OS protections:

### macOS

1. Open the `.dmg` and drag **Frame** to Applications.
2. On first launch, macOS Gatekeeper may block the app. Either:
   - Right-click the app → **Open** → confirm, or
   - **System Settings → Privacy & Security** → allow the app.

### Windows

SmartScreen may warn that the publisher is unknown. Click **More info** → **Run anyway**, or use **Unblock** on the downloaded file (file Properties → General → Unblock).

## Local packaging smoke test

On macOS or Windows:

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DPP_BROWSER_PACKAGED_BUILD=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build --prefix install
cpack --config build/CPackConfig.cmake
```

Launch the installed app and confirm the UI loads (themes, views, fonts from bundled `assets/`).

Linux GUI installs still use `bin/pp-browser` and `share/pp-browser/assets/`; CPack installers are macOS/Windows only for now.

Local **`pp-node`** image/tarball (Ubuntu 24.04 host):

```bash
sudo apt-get install -y cmake ninja-build ccache pkg-config
PP_BROWSER_RELEASE_VERSION=0.0.0-local bash scripts/pp_node_package_linux.sh all
docker build -t pp-node:local dist/pp-node/docker
```

## Checklist before tagging

- [ ] Version bumped in `CMakeLists.txt`
- [ ] `main` CI green ([`build.yml`](../.github/workflows/build.yml))
- [ ] Smoke-tested packaged build locally (if possible on target OS)
- [ ] macOS signing secrets configured (optional; unsigned OK until ready)
- [ ] GHCR package visibility set if the `pp-node` image should be pullable without auth

## Future: Windows code signing

- Import an Authenticode certificate (`.pfx`) as a secret
- Sign the NSIS installer (or the main exe before packaging) with `signtool`

Document secret names and exact commands when Windows signing is enabled.

## Deferred

| Item | Notes |
|------|-------|
| Intel macOS / universal binary | Current GHA `macos-14` is arm64 only |
| Linux `.deb` / AppImage (GUI) | Not in current target |
| `pp-node` multi-arch (`linux/arm64`) | amd64 only in release CI for now |
| Auto-update channel | Separate effort |
| iOS distribution | Separate Xcode target; see [PLATFORMS.md](../architecture/PLATFORMS.md) |
