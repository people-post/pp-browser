# In-app QR scanning

Requirement (2026-10-27): scan a friend's PP QR code and reserve a general scanning interface.
Existing C5 locator sharing and OS-camera deep links are in `projects/e2e-message-crypto/DECISIONS.md`;
`docs/contracts/ACCOUNT_CARD.md` identifies the missing in-app scanner.

Follow-up (2026-10-27): make scanning a primary action beside Me. In compact layout Me is the
Home header account button; in expanded layout it is a rail tab. Remove the old Contacts +
menu entry. Keep the scan result's explicit contact confirmation. Check the shared scanner and
own-contact QR code on Mac, iPhone and Android; distinguish builds from physical-device scans.

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

The decoder is quirc (portable C, ISC, no nonstandard dependencies), pinned and vendored with its license.
Upstream API/reference: https://github.com/dlbeer/quirc/blob/master/README.md.
No new camera stack: `MediaDeviceArbiter` still owns devices, with its existing exclusive-camera policy.

Integrated into the local main checkout while retaining the separate video-call work. Packaging
remains pending. Physical camera and mobile permission validation must be reported separately
from automated checks.
