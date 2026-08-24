# Relay blob upload — current state

**As of:** 2026-08-24 (**i1** landed)

---

## Summary

| Track | Status |
|-------|--------|
| **Planning** | Done — [DESIGN.md](DESIGN.md), [DECISIONS.md](DECISIONS.md) R001–R018 |
| **www server** | Shipped — [relay blob design](../../../web2/www/Plans/2026-08-24-relay-blob-upload-design.md) |
| **pp-browser i1** | **Done** — blob sign helpers, `HttpClient::Put`, `IBlobClient`, factory + hub wiring |
| **pp-browser i2+** | **Next** — profile icon UX |

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

1. **i2** — Me → Profile avatar pick/upload ([PHASES.md](PHASES.md#i2--profile-icon-upload-ux))
2. Use `MessagingHub::Blob()` + `UploadRelayBlobBytes` for icon flow
3. Promote blob routes to [SERVICE_ENDPOINTS.md](../../docs/contracts/SERVICE_ENDPOINTS.md) when i2 ships

---

## Still missing

| Area | Phase |
|------|-------|
| Profile icon UI | i2 |
| Directory `icon` parse + cache | i3 |
| Chat attachment wire | a1 |
| Composer attach | a2 |
