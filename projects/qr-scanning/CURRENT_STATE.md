# Current state — QR scanning

2026-10-28: the user confirmed a real PP QR reads on the iPhone 11 with a smooth preview, then
reported that a distant code is never magnified. Device logs showed 45 s of `candidates=0` before
the user moved closer; zoom hunting (below) addresses it and awaits device confirmation.

## Current behavior

- Scan is beside Me: compact Home header and expanded navigation rail. Contacts + keeps add/find.
- The preview has a centered square, dark surrounding mask, corner marks and a moving scan line.
- A PP locator leads to the existing confirmation showing nickname and Account ID; saving requires
  user confirmation. Unsupported payloads do not navigate or execute actions.
- Capture requests 1280x720 at 30 FPS for dense QR detail; the preview is bounded to 640 pixels.
  A centered detected but unreadable QR can trigger up to 3x zoom. iOS rear cameras use native
  zoom with actual progress feedback; other cameras use a software viewport. The zoom state is
  sampled before each decoded frame is fetched, so a frame captured mid-ramp can never compound a
  second magnification; off-center bounds limit the target to retain the QR.
- A code too far away produces no candidate at all (quirc needs about three pixels per module), so
  with nothing in view for 1.5 s after the zoom settles the scanner hunts 1x -> 2x -> 3x -> 1x; a
  candidate or a decode takes over, and losing a candidate for 2 s resets to 1x and hunts again.
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
- Windows CI exposed a zoom race (state read after a slow decode); fixed, and the regression now
  settles the fake ramp only on query so it fails deterministically on the old code everywhere.
  Two further reviews of the native driver kept the camera opening when a frame duration is
  rejected (macOS/UVC cameras), as upstream SDL did.

## Remaining acceptance / dependency

- Confirmed by the user on the iPhone 11: smooth preview, a real PP QR read. Reported broken on
  2026-10-28: a distant code is not magnified (no candidate detected); zoom hunting needs a device
  check. Not quantified: recognition distance, zoom reset after cancel/reopen, startup time.
  Not exercised at all: Mac and Android scanning, and the call camera on every platform after the
  driver change.
- The native fixes shipped in pp-cpp-ui `v0.6.0` ([PR #22](https://github.com/people-post/pp-cpp-ui/pull/22));
  the browser pin is that tag, so FetchContent builds (CI, no sibling checkout) carry the fix.
- Browser delivery: [PR #392](https://github.com/people-post/pp-browser/pull/392).
  No production deployment was performed.
