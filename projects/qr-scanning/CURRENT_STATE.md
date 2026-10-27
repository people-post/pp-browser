# Current state — QR scanning

2026-10-27: implementation complete per [PLAN.md](PLAN.md) and integrated into the local
main checkout alongside the separate video-call changes; packaged-device acceptance remains pending.

Already present before this work: own-contact QR generation, compact PP contact locator,
OS deep-link dispatch, pasted-invite addition and explicit confirmation for opened links.

This delivery adds in-app camera scanning, routes friend's locators to the existing confirmation,
and exposes raw QR text through caller-owned scan ports for future uses.

Verification completed:

- 8 synthetic-QR/fake-camera cases passed: decode including rotation, rear preference,
  one result then release, blocked-old-open cancellation/restart, permission/no-device errors,
  no-frame timeout, completed-result cancellation and camera already held by a call.
- 5 GUI/flow cases passed: cancel/back, background cancellation, explicit friend confirmation,
  rejection of URL/malformed contact payloads, actual scan RML mount and preview texture release
  through a CPU-only render mock.
- Changed GUI/app/media units compiled with project Release warning flags. Full desktop CMake
  configure succeeded in an isolated build directory; the quirc target built there.
- Locale JSON, Apple permission plists, vendor-script syntax and first-party diff whitespace checks passed.
  Two whitespace-only lines in the pinned quirc source are retained exactly as upstream.
- Independent review found and verified fixes for iOS motion-sensor activation and shutdown
  watchdog ordering; no confirmed issue remained after review.
- After integration, the macOS `pp-browser` application and five relevant test targets built
  successfully. All 70 focused tests passed across AI planning, people results, media, GUI and
  messaging tool registration. Combination review found no conflict with the video-call changes.

Physical Mac/iPhone/Android camera permission, GPU preview, mobile background and driver behavior
remain unverified. No package build or installation was performed.
