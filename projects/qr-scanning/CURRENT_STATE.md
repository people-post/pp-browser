# Current state — QR scanning

2026-10-28: implementation is integrated into the local main checkout alongside the separate
video-call work. Final real-QR / automatic-zoom acceptance is pending.

## Current behavior

- Scan is beside Me: compact Home header and expanded navigation rail. Contacts + keeps add/find.
- The preview has a centered square, dark surrounding mask, corner marks and a moving scan line.
- A PP locator leads to the existing confirmation showing nickname and Account ID; saving requires
  user confirmation. Unsupported payloads do not navigate or execute actions.
- Capture requests 1280x720 at 30 FPS for dense QR detail; the preview is bounded to 640 pixels.
  A centered detected but unreadable QR can trigger up to 3x zoom. iOS rear cameras use native
  zoom with actual progress feedback; other cameras use a software viewport. Pending native
  ramps cannot compound magnification, and off-center bounds limit the target to retain the QR.
- Own-contact QR generation remains Me → Profile on all three platforms, with the SVG icon tint
  overridden to preserve the QR's white background.
- Raw scan ports remain available to future callers; camera lifecycle uses the shared arbiter.

## Verification

- Earlier integrated AI/friend/scanner checks: 70 focused tests passed.
- Final zoom/capture regression: 27 media tests and 8 GUI tests passed. Coverage includes a dense
  700-byte payload, candidate→zoom→payload, off-center limits, a delayed native ramp, software
  fallback, cancellation/restart, timeout, exclusive camera ownership and identity confirmation.
- macOS app, iPhone device Release app and Android arm64 Release APK built successfully.
  The latest iPhone app was signed, installed and launched. Android wireless install was rejected
  by the phone; the new APK was not installed there.
- iPhone 11 native camera diagnosis found SDL advertising only continuous-range endpoints and
  selecting 1 FPS for a 30 FPS request, plus a quality preset replacing the requested size.
  After the pp-cpp-ui fix, native min/max frame duration was 33,333,333 ns and actual delivery
  about 30 FPS; the user confirmed smoothness. This measurement preceded the latest auto-zoom build.
- Independent reviews found off-center/ramp over-zoom and stale camera property-ID issues;
  both were corrected and reviewed. First-party diff whitespace checks pass. The pinned quirc
  source retains two upstream whitespace-only lines.

## Remaining acceptance / dependency

- Real dense/far PP QR recognition, native zoom, cancel/reopen zoom reset and final preview FPS
  still need device confirmation. Actual Mac/Android camera and call-camera behavior remain pending.
- Local builds consume pp-cpp-ui `d97b86e7` via the sibling checkout. The native fixes are in
  [pp-cpp-ui PR #22](https://github.com/people-post/pp-cpp-ui/pull/22). The browser default pin
  remains `v0.5.1`; merge/release that dependency and bump the pin before distributing the fix.
- Browser delivery: [PR #392](https://github.com/people-post/pp-browser/pull/392).
  No production deployment was performed.
