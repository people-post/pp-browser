# QR scanning

The **Scan QR code** action beside Me opens an in-app camera preview: in compact layout it is
beside the Home header's Me button; in expanded layout it is beside Me in the navigation rail.
Point it at a friend's PP
QR code; after one successful scan the scanner closes and the existing contact-invite
confirmation shows the person's name and Account ID. Only confirmation saves the contact. The
locator may carry the person's preferred relay's address — its origin and its relay ID (`relay:…`, which the relay's certificate proves)
([ACCOUNT_CARD § Locator](../contracts/ACCOUNT_CARD.md#locator-qr-code-invite-link), multi-relay MR008): the new contact
keeps it as its relay until their card arrives, so a first message can reach their mailbox, signed for the PeerId that
relay's certificate names.
Me → My relays → **Add relay…** also scans: a relay's link (`https://host#relay:…`) shown as a QR code goes into its
field and is checked as a pasted link ([UI_DESIGN_SYSTEM § My relays](UI_DESIGN_SYSTEM.md#my-relays)).
Linking a device ([LINK_DEVICE.md](../contracts/LINK_DEVICE.md)) scans too, each side its own payload, read as binary
bytes: Me → Security → Link a new device reads the new device's code, and the new device's link page (under the PIN
gate, which steps aside while the camera is open and comes back when nothing was read: `Open`'s `on_closed`) reads the
main device's reply. Neither goes through the contact / join routing below.
A group's **invite link** (`pp://join/…`, [group-chat G021](../../projects/group-chat/DECISIONS.md#g021--invite-links))
is the other kind it reads: the chats' join prompt asks the link's maker, then shows the group's name and
member count with Join / Cancel; only Join joins. Other QR contents, including ordinary URLs, produce an
unsupported-contact notice.

The camera fills the preview, with a centered square guide, a darkened surrounding area,
corner marks and an animated scan line. This guide is local UI; recognition sees the captured
frame. Scanning requests a 1920×1080 capture independently of call video settings (a camera
without it delivers the closest size it has): the link handshake's codes are QR version 29
(141 modules with the quiet zone), which need about 3 pixels per module. Recognition keeps
frames up to 1920 on their long edge whole, and quirc is built with 16-bit region numbers
(`third_party/quirc/CMakeLists.txt`): at its default of 254 blobs it never saw the finder
pattern at the bottom of a dense code. A decode takes about 11 ms at 1080p on a Mac and runs on
the scanner's worker, never more than half its time.

**Scan only where it can work.** `QrScanController::CanScan()` asks the media backend whether the OS
lists a camera (`IMediaDeviceBackend::HasCamera`; nothing is opened, no permission prompt). The
link pages offer *Scan* only then; with no camera, *Paste* is their one main button.

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
locator handling through `OfferInvite`, and hands a `pp://join/…` payload first to the chats'
`GroupJoinPrompt` (`ContactsController::BindGroupJoinOffer` → `ChatController::OfferGroupJoin`).

**Where a link arrives.** One rule for every entry: a `pp://join/…` anywhere in the text goes to the
join prompt (`group_invite_link::Find`), else a contact locator to Contacts — the in-app scan, text
pasted into *Add contact*, and a link the OS opens the app with (`IncomingLink`: the app registers
the `pp` scheme with host `join` beside `pp-browser-contact-v1` — iOS / macOS `CFBundleURLTypes`, an
Android `VIEW` intent filter). A group's link is shown as text and as a QR code in group info's
*Invite link…* sheet (`lib/ui/QrSvg`, as the contact QR).

`QrScanner` uses a worker and `MediaDeviceArbiter` camera lease, preferring the rear camera. Generation checks discard results from a canceled
or replaced scan. quirc decodes the QR; `ElementScanPreview` owns the preview texture within
the document, independently of call video tiles.

Shutdown joins the scanner before UI/device/SDL teardown, after the product's exit watchdog
is armed and the window hidden, because an OS camera open may still be inside a driver.
No microphone or audio session is requested for scanning.
