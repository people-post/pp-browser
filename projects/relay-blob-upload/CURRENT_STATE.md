# Relay blob upload — current state

**As of:** 2026-08-24 (**i2** landed)

---

## Summary

| Track | Status |
|-------|--------|
| **Planning** | Done — [DESIGN.md](DESIGN.md), [DECISIONS.md](DECISIONS.md) R001–R018 |
| **www server** | Shipped — [relay blob design](../../../web2/www/Plans/2026-08-24-relay-blob-upload-design.md) |
| **pp-browser i1** | **Done** — blob sign helpers, `HttpClient::Put`, `IBlobClient`, factory + hub wiring |
| **pp-browser i2** | **Done** — Me → Profile avatar pick/upload/clear |
| **pp-browser i3+** | **Next** — directory icon parse + contacts/call render |

---

## What landed (i2)

| Component | Path |
|-----------|------|
| Self icon cache | `src/base/people/ProfileIconCache.*` |
| Image prep | `src/base/platform/ProfileIconImagePrep.*` |
| File dialog | `src/base/platform/NativeFileDialog.*` |
| Upload util | `src/base/net/ProfileIconClientUtil.*` |
| Hub / facade | `MessagingHub::UploadProfileIconFromPath`, `ClearProfileIcon` |
| Settings UI | `settings_section_profile.rml`, `SettingsController`, `ProfileSettingsSection` |
| i18n | `settings.profile.change_photo`, `settings.profile.remove_photo` |

---

## What landed (i1)

| Component | Path |
|-----------|------|
| Sign bytes | `src/base/net/RelayBlobSignPayload.*` |
| HTTP PUT | `src/base/net/HttpClient.*` |
| Blob client API | `src/base/net/BlobClient.*`, `HttpBlobClient.*` |
| Mock + upload helper | `MockBlobClient` in `ServiceClientsImpl.*`, `UploadRelayBlobBytes` |
| Factory / hub | `ServiceClientFactory`, `MessagingHub::Blob()` + auth signer |
| Tests | `relay_blob_sign_payload_test.cpp`, `blob_client_test.cpp` |

---

## Next agent — start here

1. **i3** — Parse directory `icon`, fetch/cache remote icons, render in contacts/call chrome ([PHASES.md](PHASES.md#i3--icon-cache--render))
2. Manual smoke: Me → Profile → pick image → upload → clear (requires registered relay user)
3. Promote blob routes to [SERVICE_ENDPOINTS.md](../../docs/contracts/SERVICE_ENDPOINTS.md)

---

## Still missing

| Area | Phase |
|------|-------|
| Directory `icon` parse + remote cache | i3 |
| Contacts / call chrome icon render | i3 |
| Chat attachment wire | a1 |
| Composer attach | a2 |
