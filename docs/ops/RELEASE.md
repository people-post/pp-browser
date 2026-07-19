# Releasing pp-browser

**Tier:** ops

Tag-triggered CI builds macOS and Windows installers, an Android release APK, and publishes them to [GitHub Releases](https://github.com/people-post/pp-browser/releases).

When [macOS signing secrets](#macos-code-signing-and-notarization) are configured, release CI code-signs and notarizes the macOS DMG. Until then, macOS artifacts ship unsigned (Gatekeeper override required).

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
5. When all jobs succeed, a GitHub Release is created with the artifacts attached.

Release CI uses the same OS runners, Android NDK, and compiler-cache setup as [build CI](../.github/workflows/build.yml). Linux desktop packages are not published.

Release builds use:

- `-DPP_BROWSER_PACKAGED_BUILD=ON` — runtime asset paths relative to the installed executable / bundle

## Artifacts

| Platform | File | Contents |
|----------|------|----------|
| macOS (Apple Silicon) | `frame-<version>-macos.dmg` | Drag-and-drop install of `Frame.app` |
| Windows x64 | `frame-<version>-windows-x64.exe` | NSIS installer (exe + `assets/` under install dir) |
| Android | `pp-browser-<version>-android.apk` | Universal APK (`armeabi-v7a`, `arm64-v8a`, `x86_64`); signed with the debug keystore until a release keystore is configured |

## macOS code signing and notarization

Infrastructure lives under [`packaging/macos/`](../../packaging/macos/) and [`scripts/macos_sign_and_notarize.sh`](../../scripts/macos_sign_and_notarize.sh).

Release CI runs signing **after** `cmake --install` and **before** `cpack`, then notarizes and staples the `.dmg`. Steps skip gracefully when secrets are not set (unsigned DMG, same as before).

### Apple Developer Portal (one-time)

1. Register bundle ID **`dev.frame.app`** (or update [`src/app/CMakeLists.txt`](../../src/app/CMakeLists.txt) if you use a different ID).
2. Create a **Developer ID Application** certificate (distribution outside the Mac App Store).
3. Create an **App Store Connect API key** with notarization access; download the `.p8` file.
4. Note your **Team ID** (10 characters).

### GitHub repository secrets

Add these under **Settings → Secrets and variables → Actions** (replace placeholders with real values):

| Secret | Example / placeholder | Purpose |
|--------|----------------------|---------|
| `APPLE_CERTIFICATE_BASE64` | Base64 of exported `.p12` | Developer ID Application cert + private key |
| `APPLE_CERTIFICATE_PASSWORD` | `YOUR_P12_PASSWORD` | `.p12` export password |
| `APPLE_SIGNING_IDENTITY` | `Developer ID Application: Your Org (YOUR_TEAM_ID)` | Exact Keychain identity string |
| `APPLE_TEAM_ID` | `YOUR_TEAM_ID` | Team ID (reference / future checks) |
| `APPLE_NOTARY_KEY_ID` | `YOUR_NOTARY_KEY_ID` | App Store Connect API key ID |
| `APPLE_NOTARY_ISSUER_ID` | `YOUR_NOTARY_ISSUER_ID` | App Store Connect issuer ID |
| `APPLE_NOTARY_P8_BASE64` | Base64 of `AuthKey_*.p8` | Notarization API private key |

To base64-encode files locally:

```bash
base64 -i DeveloperIDApplication.p12 | pbcopy
base64 -i AuthKey_XXXX.p8 | pbcopy
```

### Local smoke test (macOS)

Copy the example env file and fill in placeholders:

```bash
cp packaging/macos/signing.env.example packaging/macos/signing.env
# edit signing.env — paths and YOUR_* placeholders
source packaging/macos/signing.env

cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DPP_BROWSER_PACKAGED_BUILD=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build --prefix install

./scripts/macos_sign_and_notarize.sh sign-app install/Frame.app
(cd build && cpack -G DragNDrop)
dmg="$(find build -maxdepth 2 -name '*.dmg' -print -quit)"
./scripts/macos_sign_and_notarize.sh notarize "$dmg"
./scripts/macos_sign_and_notarize.sh staple "$dmg"
```

Entitlements: [`packaging/macos/Frame.entitlements`](../../packaging/macos/Frame.entitlements) (network + hardened-runtime flags for Brief / libp2p). Adjust if notarization logs request additional entitlements.

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

Linux dev installs still use `bin/pp-browser` and `share/pp-browser/assets/`; CPack installers are macOS/Windows only for now.

## Checklist before tagging

- [ ] Version bumped in `CMakeLists.txt`
- [ ] `main` CI green ([`build.yml`](../.github/workflows/build.yml))
- [ ] Smoke-tested packaged build locally (if possible on target OS)
- [ ] macOS signing secrets configured (optional; unsigned OK until ready)

## Future: Windows code signing

- Import an Authenticode certificate (`.pfx`) as a secret
- Sign the NSIS installer (or the main exe before packaging) with `signtool`

Document secret names and exact commands when Windows signing is enabled.

## Deferred

| Item | Notes |
|------|-------|
| Intel macOS / universal binary | Current GHA `macos-14` is arm64 only |
| Linux `.deb` / AppImage | Not in current target (no Linux desktop release artifact) |
| Auto-update channel | Separate effort |
| iOS distribution | Separate Xcode target; see [PLATFORMS.md](../architecture/PLATFORMS.md) |
