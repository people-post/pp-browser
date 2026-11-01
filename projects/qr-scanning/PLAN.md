# In-app QR scanning

Goal: scan a friend's PP QR code, show identity before the user adds the contact, and expose
raw scan ports for future features. The primary entry sits beside Me in compact Home and the
expanded rail. The preview keeps a full camera view with a central square, surrounding mask,
corner marks and moving scan line. Distant detected codes should magnify automatically.

Current implementation and verification status: [CURRENT_STATE.md](CURRENT_STATE.md).

## Delivery and acceptance

| Work | Acceptance |
|------|------------|
| Capture | Shared exclusive camera arbiter; rear preference; native frame rate measured, not inferred from SDL's configured spec; cancellation/restart isolates generations |
| Decode | Detailed 1280x720 capture, bounded quirc decoding off UI, dense locator and rotation coverage; displayed preview bounded to 640 pixels |
| Magnification | Centered unreadable candidate → up to 3x; retain bounds/quiet zone; wait for actual native ramp progress; restore 1x on candidate loss/close; software fallback when native control is absent |
| UI | Scan beside Me; full view with central guide/mask/scan line; permission/error/retry/cancel; camera released on close, background, profile reset and shutdown |
| Friend action | Valid PP locator → existing nickname/Account ID confirmation; only the user's confirmation saves; ordinary URLs and malformed codes never execute |
| Extension | Caller-owned ScanPorts receive one raw payload after the scanner closes; no contact/payment/URL semantics in the decoder |
| Verification | Dense QR decode, fake capture/cancel/timeout/exclusivity, delayed native ramp and off-center regression, software fallback, UI identity confirmation; compile Mac/iPhone/Android, then actual device scan and reopen |

## Dependency and evidence

quirc is pinned/vendored with ISC license and import script. Reference:
https://github.com/dlbeer/quirc/blob/master/README.md.

Apple capture fixes and zoom controls live in pp-cpp-ui, not an app-owned camera stack.
The native driver must advertise common rates inside continuous ranges and preserve the chosen
format after session inputs attach. [UI PR #22](https://github.com/people-post/pp-cpp-ui/pull/22)
must be merged/released and the browser pin bumped before distribution; local device builds
consume the sibling checkout.

Physical acceptance checks actual delivered FPS, first-frame latency, real dense/far QR identity,
zoom/cancel/reopen, and camera coexistence with calls. Automated and device results remain distinct.
Past verification misses (theme tint on QR white, FPS from native delivery not SDL config, zoom
ramp feedback vs wall-clock fakes, reopen invalidating camera property IDs) are recorded under
[CURRENT_STATE.md](CURRENT_STATE.md) Verification — keep regressions there, not a parallel lessons file.
