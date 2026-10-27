# QR scanning

The **Scan QR code** action beside Me opens an in-app camera preview: in compact layout it is
beside the Home header's Me button; in expanded layout it is beside Me in the navigation rail.
Point it at a friend's PP
QR code; after one successful scan the scanner closes and the existing contact-invite
confirmation shows the person's name and Account ID. Only confirmation saves the contact.
Other QR contents, including ordinary URLs, produce an unsupported-contact notice.

The camera fills the preview, with a centered square guide, a darkened surrounding area,
corner marks and an animated scan line. This guide is local UI; recognition sees the captured
frame. Scanning requests a modest capture size independently of call video settings.

A detected but unreadable centered code can trigger automatic magnification up to 3x. Native
iOS rear-camera zoom waits for actual ramp progress before increasing again; unsupported
cameras use a software viewport. The target retains the code and its quiet-zone margin, and
losing the candidate for two seconds restores 1x. Recognition uses detailed capture frames
while the displayed preview is downsampled, so preview texture uploads stay bounded.

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
