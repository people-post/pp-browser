# Relay blob upload — current state

**As of:** 2026-08-24 (**i3** landed)

---

## Summary

| Track | Status |
|-------|--------|
| **Planning** | Done — [DESIGN.md](DESIGN.md), [DECISIONS.md](DECISIONS.md) R001–R018 |
| **www server** | Shipped — [relay blob design](../../../web2/www/Plans/2026-08-24-relay-blob-upload-design.md) |
| **pp-browser i1** | **Done** — blob sign helpers, `HttpClient::Put`, `IBlobClient`, factory + hub wiring |
| **pp-browser i2** | **Done** — Me → Profile avatar pick/upload/clear |
| **pp-browser i3** | **Done** — directory `icon` parse, peer cache, contacts/call render |
| **pp-browser a1+** | **Next** — chat attachment wire |

---

## What landed (i3)

| Component | Path |
|-----------|------|
| `ProfileIconRef` on hits/contacts | `ContactTypes.h`, `ContactJson.cpp`, `ContactsStore.cpp` |
| Per-peer cache keys | `ProfileIconCache.*` (`cache/icons/{relay\|account}/`) |
| CDN fetch | `ProfileIconFetchUtil.*` (`HttpClient::Get` + stale detection) |
| Hub hooks | `MessagingHub::Ensure*IconCached`, `DirectoryShadowCache::SetOnHitCached` |
| Contacts UI | `contacts.rml`, `contact_detail.rml`, `ContactsController` |
| Call chrome | `CallRosterParticipantState.avatar_src`, immersive roster `<img>` |
| Contact card | `InboxController::BuildContactCardRml` renders `avatar_url` |

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

1. **a1** — `ChatContentType::Attachment` wire + codec ([PHASES.md](PHASES.md#a1--attachment-wire--codec))
2. Promote blob routes to [SERVICE_ENDPOINTS.md](../../docs/contracts/SERVICE_ENDPOINTS.md)
3. Manual smoke: sync contact from directory with icon → avatar in list/detail/call

---

## Still missing

| Area | Phase |
|------|-------|
| Chat attachment wire | a1 |
| Composer attach | a2 |
| Attachment bubbles + quota UX | a3–a4 |
