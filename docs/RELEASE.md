# Releasing pp-browser

Tag-triggered CI builds unsigned macOS and Windows installers and publishes them to [GitHub Releases](https://github.com/people-post/pp-browser/releases).

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
   - **macOS**: builds `pp-browser.app`, packages a `.dmg`
   - **Windows**: builds the app, packages an NSIS `.exe` installer
5. When both jobs succeed, a GitHub Release is created with both artifacts attached.

Release builds use:

- `-DPP_BROWSER_PACKAGED_BUILD=ON` — runtime asset paths relative to the installed executable / bundle

## Artifacts

| Platform | File | Contents |
|----------|------|----------|
| macOS (Apple Silicon) | `pp-browser-<version>-macos.dmg` | Drag-and-drop install of `pp-browser.app` |
| Windows x64 | `pp-browser-<version>-windows-x64.exe` | NSIS installer (exe + `assets/` under install dir) |

## Installing unsigned builds

Artifacts are **not code-signed** in the current pipeline. Users may need to override OS protections:

### macOS

1. Open the `.dmg` and drag `pp-browser` to Applications.
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

## Future: code signing

When certificates are available, extend the release workflow:

### macOS

- Import signing certificate and notarization credentials as GitHub secrets
- After `cmake --install`, run `codesign` on `pp-browser.app`
- Submit to Apple with `notarytool`, staple the ticket, then run `cpack`

### Windows

- Import an Authenticode certificate (`.pfx`) as a secret
- Sign the NSIS installer (or the main exe before packaging) with `signtool`

Document secret names and exact commands when signing is enabled.

## Deferred

| Item | Notes |
|------|-------|
| Intel macOS / universal binary | Current GHA `macos-latest` is arm64 only |
| Linux `.deb` / AppImage | Not in current target |
| Auto-update channel | Separate effort |
