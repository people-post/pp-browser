# QR scanning

Contacts → **+ → Scan QR code** opens an in-app camera preview. Point it at a friend's PP
QR code; after one successful scan the scanner closes and the existing contact-invite
confirmation shows the person's name and Account ID. Only confirmation saves the contact.
Other QR contents, including ordinary URLs, produce an unsupported-contact notice.

The camera opens only for an active scan. Cancel, back/scrim dismissal, mobile background,
window minimize/hide, profile reset and application shutdown stop the scan. A camera in use
by a call is refused through the existing exclusive camera policy; scanning never takes it
away. Permission denial, open failure and 15 seconds without frames show a retryable error.
Frames are kept locally in memory; scanning does not save or upload images.

## Extension interface

`gui/scan/ScanPorts.h` exposes `scan_qr(QrScanResultCallback)`. The callback receives one raw
decoded payload after the scan surface has closed. Cancel or capture failure does not deliver
a payload. A new caller can reuse this entry and interpret supported payloads in its own
feature. The scanner does not navigate URLs, add contacts or execute decoded commands.

Application owns `QrScanController` and binds its ports; Contacts supplies the caller-specific
locator handling through `OfferInvite`. `QrScanner` uses a worker and `MediaDeviceArbiter`
camera lease, preferring the rear camera. Generation checks discard results from a canceled
or replaced scan. quirc decodes the QR; `ElementScanPreview` owns the preview texture within
the document, independently of call video tiles.

Shutdown joins the scanner before UI/device/SDL teardown, after the product's exit watchdog
is armed and the window hidden, because an OS camera open may still be inside a driver.
No microphone or audio session is requested for scanning.
