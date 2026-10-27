# In-app QR scanning

Requirement (2026-10-27): scan a friend's PP QR code and reserve a general scanning interface.
Existing C5 locator sharing and OS-camera deep links are in `projects/e2e-message-crypto/DECISIONS.md`;
`docs/contracts/ACCOUNT_CARD.md` identifies the missing in-app scanner.

Follow-up (2026-10-27): make scanning a primary action beside Me. In compact layout Me is the
Home header account button; in expanded layout it is a rail tab. Remove the old Contacts +
menu entry. Keep the scan result's explicit contact confirmation. Check the shared scanner and
own-contact QR code on Mac, iPhone and Android; distinguish builds from physical-device scans.

Device feedback (2026-10-27): the own QR appears too dark, and the scan camera preview stutters.
The global SVG icon tint also affects the embedded QR image; override its tint to white. The
scanner's fixed 90 ms wait after every captured frame caps preview updates below camera FPS;
poll frames sooner and decode less often than preview delivery. Verify the resulting QR contrast
and live preview on device, separately from automated scan tests.

The first cadence change did not resolve device feedback. Follow-up: request 30 FPS instead of
15, wake the scan surface at 60 Hz instead of 20, install an optimized Release iPhone build,
and log capture FPS / maximum decode time to distinguish remaining capture and decode stalls.

The Release device log still showed only ~2 captured frames/second although SDL reported 30 FPS;
decode took 2–3 ms and the first frame took ~2.5 seconds. Add capture polling/time/size metrics
to isolate native delivery versus conversion. Request a scan-only 480×360 capture, use a compact
300 dp-high viewfinder, and draw a local moving scan line. Full-frame QR recognition stays active.

## Delivery

| Step | Acceptance | Status |
|------|------------|--------|
| Capture and decode | Shared camera arbiter; prefer rear camera; bounded QR decoding off UI; cancellation/restart isolation | Implemented; 8 automated cases passed |
| Scan surface | Primary Scan QR code action beside Me; preview, status, retry and cancel/back; release camera on result/close/background/shutdown | Entry implemented and seen on iPhone; fake lifecycle and CPU-render checks passed; device QR recognition pending |
| Friend action | PP locator → existing explicit Add contact confirmation; other codes explain unsupported friend QR; never follow URLs automatically | Implemented; confirmation/rejection cases passed |
| Extension seam | Scan ports return raw decoded text to their caller; scanner knows no contact/payment/URL actions | Implemented; app-bound ScanPorts |
| Validation and review | Synthetic QR decode; fake camera lifecycle; UI/flow regressions; app build; independent review | 13 scanner-specific cases passed; integrated macOS app build and 70 focused tests passed; review fixes verified |
| Primary entry follow-up | Scan button beside Me in compact Home header and expanded rail; same contact confirmation; remove Contacts + menu entry | Implemented; Mac/iPhone/Android builds passed, iPhone Home entry seen; tap-to-scan on revised entry pending device check |
| Cross-platform follow-up | Confirm own QR generation and in-app scanner are present in shared code, then check packages and device behavior separately | Shared code confirmed; iPhone app installed/launched and rear camera delivered a frame; Android arm64 APK built but device refused install; actual scans and Mac/Android camera checks pending |
| Device feedback fixes | Keep own QR white/black under theme tint; publish previews at camera cadence and decode at a lower rate | Implemented; Mac/iPhone/Android builds and 10 scanner/QR tests passed; fixed Mac/iPhone apps installed; visual/device acceptance pending |

The decoder is quirc (portable C, ISC, no nonstandard dependencies), pinned and vendored with its license.
Upstream API/reference: https://github.com/dlbeer/quirc/blob/master/README.md.
No new camera stack: `MediaDeviceArbiter` still owns devices, with its existing exclusive-camera policy.

Integrated into the local main checkout while retaining the separate video-call work. Packaging
remains pending. Physical camera and mobile permission validation must be reported separately
from automated checks.
