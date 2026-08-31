# iOS build and signing

**Tier:** ops

How to build **PP** for iOS (simulator and device) and connect an **Apple Developer Program** account for on-device testing. User-visible product is **PP** (`PP.app`); signing / reverse-DNS ID is **`dev.pp-browser.ios`** ([PRODUCT_BRANDING.md](../ui/PRODUCT_BRANDING.md)).

**Related:** [BUILD.md](BUILD.md), [PLATFORMS.md](../architecture/PLATFORMS.md), [MACOS_SIGNING.md](MACOS_SIGNING.md) (macOS Developer ID — separate from iOS provisioning), [PRODUCT_BRANDING.md](../ui/PRODUCT_BRANDING.md).

---

## Overview

| What | Value in this repo |
|------|-------------------|
| iOS app bundle | `PP.app` |
| Bundle ID | `dev.pp-browser.ios` ([`packaging/ios/Info.plist`](../../packaging/ios/Info.plist), [`cmake/IosBundle.cmake`](../../cmake/IosBundle.cmake)) |
| Build script | [`scripts/ios_build.sh`](../../scripts/ios_build.sh) |
| Signing script | [`scripts/ios_sign.sh`](../../scripts/ios_sign.sh) |
| Entitlements | [`packaging/ios/pp-browser.entitlements`](../../packaging/ios/pp-browser.entitlements) |
| Local env template | [`packaging/ios/signing.env.example`](../../packaging/ios/signing.env.example) |
| Export template | [`packaging/ios/ExportOptions.plist.example`](../../packaging/ios/ExportOptions.plist.example) |

iOS builds require **macOS + Xcode**. Linux CI can cross-compile some dependencies, but producing a runnable `.app` is macOS-only today.

Until you fill in signing placeholders, **simulator builds work unsigned**; **device installs require** a development certificate and provisioning profile.

---

## Prerequisites

- macOS with **Xcode matching the device iOS major** (e.g. iPhone on **iOS 26.5** needs **Xcode 26.5+**; Xcode 26.5+ itself needs **macOS Tahoe 26.2+**)
- **CMake 3.24+**, **Ninja** (recommended)
- Vendored trees present (`./scripts/vendor_import.sh`, `./scripts/libp2p_vendor_import.sh` if needed)

Install command-line tools if needed:

```bash
xcode-select --install
```

If `xcrun devicectl` reports **developer disk image could not be mounted**, the Mac’s Xcode is older than the phone’s iOS — update Xcode (and macOS if required) before device install/debug will work.

---

## Quick start (simulator)

From the repository root:

```bash
chmod +x scripts/ios_build.sh scripts/ios_sign.sh   # once, if not executable
./scripts/ios_build.sh sim
```

Open **Simulator.app**, then:

```bash
./scripts/ios_build.sh run-sim
```

On first launch, open **Me → Assistant** and enter a cloud API key (same as Android/desktop).

### Soft keyboard (Simulator)

Focusing a text field calls `SDL_StartTextInput` → UIKit `becomeFirstResponder`. That is enough for a **device**. On the **Simulator**, macOS’s hardware keyboard is usually “connected”, and iOS hides the software keyboard.

To show it:

- **I/O → Keyboard → Toggle Software Keyboard** (or **⌘K**)
- Or uncheck **I/O → Keyboard → Connect Hardware Keyboard** so the soft keyboard appears whenever a field is focused

If typing with the Mac keyboard works but nothing appears on screen, the app path is fine — only the Simulator soft-keyboard visibility is off.

---

## Build commands

```bash
./scripts/ios_build.sh configure-sim     # CMake → build-ios-simulator/
./scripts/ios_build.sh configure-device  # CMake → build-ios-device/
./scripts/ios_build.sh build             # Build current tree
./scripts/ios_build.sh install           # Install to install-ios/PP.app
./scripts/ios_build.sh sim               # configure + build + install (simulator)
./scripts/ios_build.sh device            # configure + build + install (device)
./scripts/ios_build.sh run-sim           # install + launch on Simulator
./scripts/ios_build.sh run-device        # sign (from signing.env) + install + launch on iPhone
./scripts/ios_build.sh xcode             # -G Xcode for IDE debugging
./scripts/ios_build.sh clean             # Remove build-ios-* trees
```

Optional version metadata:

```bash
export PP_BROWSER_VERSION=0.1.0
export PP_BROWSER_RELEASE_VERSION=0.1.0-rc1
./scripts/ios_build.sh sim
```

Manual CMake (equivalent to `configure-sim`):

```bash
cmake -B build-ios-simulator -S . \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPP_BROWSER_PACKAGED_BUILD=ON \
  -G Ninja
cmake --build build-ios-simulator -j
cmake --install build-ios-simulator --prefix install-ios
```

The first clean iOS build can take **15–30 minutes** (libp2p + RmlUi + BoringSSL), similar to Android NDK.

---

## One-time: Apple Developer Portal

Do this at [developer.apple.com/account](https://developer.apple.com/account) after enrolling in the Apple Developer Program.

### 1. Register the iOS bundle ID

1. **Certificates, Identifiers & Profiles → Identifiers → App IDs**
2. Register **`dev.pp-browser.ios`** (or your org ID, e.g. `com.yourorg.pp-browser.ios`)
3. If you change the ID, update:
   - [`packaging/ios/Info.plist`](../../packaging/ios/Info.plist)
   - `PP_BROWSER_IOS_BUNDLE_ID` in [`cmake/IosBundle.cmake`](../../cmake/IosBundle.cmake)
   - [`packaging/ios/signing.env.example`](../../packaging/ios/signing.env.example)
   - Entitlements / export templates under [`packaging/ios/`](../../packaging/ios/)
4. Pre-release installs using `dev.frame.ios` are not migrated — delete the old app and reinstall.

### 2. Create an iOS Development certificate

1. **Certificates → +**
2. Choose **Apple Development** (for device testing) or **Apple Distribution** (TestFlight/App Store)
3. Create a CSR in Keychain Access → upload → download cert → double-click to install

### 3. Create a provisioning profile

1. **Profiles → + → iOS App Development** (or Ad Hoc / App Store)
2. Select App ID **`dev.pp-browser.ios`**
3. Select your development certificate and test devices
4. Download **`pp-browser_iOS_Development.mobileprovision`**

### 4. Note your Team ID

**Membership details → Team ID** (10 characters, e.g. `AB12CD34EF`).

### 5. Optional: APNs key (push — deferred)

Push notifications are not implemented yet ([`projects/push-notifications/`](../../projects/push-notifications/)). When ready:

1. **Keys → + → Apple Push Notifications service (APNs)**
2. Download **`AuthKey_XXXX.p8`** (once)
3. Fill `IOS_APNS_*` placeholders in `signing.env.example`

---

## Local signing setup

```bash
cp packaging/ios/signing.env.example packaging/ios/signing.env
# Edit signing.env — replace YOUR_* placeholders
source packaging/ios/signing.env
```

| Variable | Placeholder | Purpose |
|----------|-------------|---------|
| `IOS_BUNDLE_IDENTIFIER` | `dev.pp-browser.ios` | Must match App ID |
| `IOS_DEVELOPMENT_TEAM` | `YOUR_TEAM_ID` | 10-character Team ID |
| `IOS_SIGNING_IDENTITY` | `Apple Development: …` | From Keychain |
| `IOS_PROVISIONING_PROFILE_PATH` | `packaging/ios/*.mobileprovision` | Development profile (gitignored) |

Sign + install on a connected iPhone:

```bash
./scripts/ios_build.sh device          # once, or when sources change
source packaging/ios/signing.env
./scripts/ios_build.sh run-device      # embeds profile, codesigns, devicectl install + launch
```

Or manually: `./scripts/ios_sign.sh sign-app install-ios/PP.app` then  
`xcrun devicectl device install app --device <UDID> install-ios/PP.app`.

---

## TestFlight / App Store Connect

Development USB install and TestFlight use **different** certificates and profiles.

| Path | Certificate | Profile |
|------|-------------|---------|
| `run-device` | Apple **Development** | iOS App **Development** (device UDIDs) |
| TestFlight IPA | Apple **Distribution** | **App Store** (same App ID `dev.pp-browser.ios`) |

### Portal one-time (while setting up ASC)

1. **Certificates → Apple Distribution** — CSR → install `.cer` in Keychain.
2. **Profiles → App Store** for App ID `dev.pp-browser.ios` — download `.mobileprovision` into `packaging/ios/` (gitignored).
3. [App Store Connect](https://appstoreconnect.apple.com) → create iOS app with bundle ID `dev.pp-browser.ios`.
4. Optional upload API: **Users and Access → Integrations → App Store Connect API** → create a key; save `AuthKey_*.p8`, Key ID, Issuer ID.

Confirm identities:

```bash
security find-identity -v -p codesigning
```

### Local `signing.env` (distribution block)

```bash
cp packaging/ios/ExportOptions.plist.example packaging/ios/ExportOptions.plist
# Edit ExportOptions.plist: teamID + provisioningProfiles name
# (portal Name, e.g. pp-browser-ios-dist — not the filename)
```

In `packaging/ios/signing.env`:

```bash
IOS_EXPORT_METHOD=app-store
IOS_DISTRIBUTION_SIGNING_IDENTITY="Apple Distribution: YOUR_ORG (TEAMID)"
IOS_DISTRIBUTION_PROVISIONING_PROFILE_PATH=packaging/ios/pp-browser_iOS_App_Store.mobileprovision
PP_BROWSER_VERSION=0.1.0
PP_BROWSER_BUILD_NUMBER=1   # bump every upload
# Optional Transporter alternative:
# IOS_ASC_KEY_ID=...
# IOS_ASC_ISSUER_ID=...
# IOS_ASC_P8_PATH=/path/to/AuthKey_....p8
```

### Build, export, upload

```bash
./scripts/ios_build.sh ipa              # Release device build + dist-ios/*.ipa
./scripts/ios_build.sh upload-ipa       # or open dist-ios/ in Transporter.app
```

`ipa` forces `IOS_EXPORT_METHOD=app-store` if unset, signs with Distribution + App Store profile, uses a secure codesign timestamp, strips `get-task-allow`, and stamps `CFBundleShortVersionString` / `CFBundleVersion` from the env vars above. Each App Store Connect upload needs a **new** `PP_BROWSER_BUILD_NUMBER`.

### After upload

1. Wait for build processing in App Store Connect.
2. Answer **export compliance** — cheat sheet: [APP_STORE_EXPORT_COMPLIANCE.md](APP_STORE_EXPORT_COMPLIANCE.md). Info.plist already sets `ITSAppUsesNonExemptEncryption=true`.
3. **TestFlight → Internal Testing** → add group / testers → enable the build.
4. External testing needs Beta App Review + more listing metadata.

**Encryption / export compliance** (questionnaire, crypto inventory, French declaration if France is available): see [APP_STORE_EXPORT_COMPLIANCE.md](APP_STORE_EXPORT_COMPLIANCE.md).

---

## GitHub Actions secrets (optional CI)

When you add an iOS release job later, typical secrets mirror the local env:

| Secret | Purpose |
|--------|---------|
| `IOS_CERTIFICATE_BASE64` | Base64 of signing `.p12` (Distribution for TestFlight) |
| `IOS_CERTIFICATE_PASSWORD` | `.p12` export password |
| `IOS_PROVISIONING_PROFILE_BASE64` | Base64 of App Store `.mobileprovision` |
| `IOS_SIGNING_IDENTITY` / `IOS_DISTRIBUTION_SIGNING_IDENTITY` | Exact codesign identity string |
| `IOS_DEVELOPMENT_TEAM` | Team ID |
| `IOS_BUNDLE_IDENTIFIER` | `dev.pp-browser.ios` |
| `IOS_ASC_KEY_ID` / `IOS_ASC_ISSUER_ID` / `IOS_ASC_P8_BASE64` | App Store Connect API upload |

No iOS release workflow is wired yet — macOS release CI remains in [`.github/workflows/release.yml`](../../.github/workflows/release.yml).

---

## Architecture notes

| Component | iOS behavior |
|-----------|--------------|
| Renderer | OpenGL ES 3 via SDL (same pattern as Android) |
| Assets | `PP.app/assets/` — staged by [`cmake/IosBundle.cmake`](../../cmake/IosBundle.cmake) |
| Paths | [`IosPathProvider`](../../src/base/platform/IosPathProvider.cpp) — SDL pref path under sandbox |
| MCP | HTTP URL only — no subprocess on mobile |
| libp2p | PeerId + key wire only (A017); no Host/protoc bootstrap |
| Keychain / APNs | Placeholder entitlements; implementation deferred |

See [PLATFORMS.md](../architecture/PLATFORMS.md) for lifecycle and GL reset behavior (mirror Android where applicable).

---

## Troubleshooting

| Symptom | Likely fix |
|---------|------------|
| `iOS builds require macOS` | Run on a Mac; simulator/device tooling is not available on Linux agents |
| Codesign / profile mismatch | Ensure App ID, profile, and `IOS_BUNDLE_IDENTIFIER` all match |
| Instant quit on open (older iPhone) | Binary `minos` must match deployment target. Check with `vtool -show-build PP.app/PP` — if `minos` is the SDK (e.g. 18.0) instead of `15.0`, reconfigure with `CMAKE_OSX_DEPLOYMENT_TARGET` (see `scripts/ios_build.sh`) and rebuild. Xcode attribute alone does not affect Ninja. |
| `0xe800003a` / could not be verified | App was signed without `application-identifier`. `ios_sign.sh` must extract entitlements from the `.mobileprovision` (fixed via `plutil -extract`); re-run `sign-app` / `run-device`. |
| ASC rejects IPA / Invalid Signature | Use `IOS_EXPORT_METHOD=app-store`, **Distribution** identity, **App Store** profile (not Development). Confirm `codesign -d --entitlements :-` has no `get-task-allow`. |
| ASC rejects reused version | Bump `PP_BROWSER_BUILD_NUMBER` (CFBundleVersion) every upload |
| `altool` cannot find API key | Place `AuthKey_<KEY_ID>.p8` via `IOS_ASC_P8_PATH`, or upload with Transporter |
| Blank window / GL error | Confirm `UIRequiredDeviceCapabilities` includes `opengles-3`; check device logs in Xcode |
| Missing assets | Re-run `cmake --build` so POST_BUILD asset copy runs; verify `PP.app/assets/` |

---

## Quick checklist

- [ ] Xcode + command-line tools installed
- [ ] `./scripts/ios_build.sh sim` succeeds
- [ ] Simulator launch via `./scripts/ios_build.sh run-sim`
- [ ] App ID `dev.pp-browser.ios` registered (or plist/CMake updated)
- [ ] Development cert + provisioning profile created
- [ ] `packaging/ios/signing.env` filled from example
- [ ] `./scripts/ios_sign.sh sign-app install-ios/PP.app` verifies on device
- [ ] Apple Distribution cert + App Store provisioning profile
- [ ] App Store Connect iOS app for `dev.pp-browser.ios`
- [ ] `IOS_DISTRIBUTION_*` + `IOS_EXPORT_METHOD=app-store` in `signing.env`
- [ ] `./scripts/ios_build.sh ipa` → `dist-ios/*.ipa`
- [ ] Upload (script or Transporter) + export compliance + Internal TestFlight
- [ ] Encryption answers / docs per [APP_STORE_EXPORT_COMPLIANCE.md](APP_STORE_EXPORT_COMPLIANCE.md)
