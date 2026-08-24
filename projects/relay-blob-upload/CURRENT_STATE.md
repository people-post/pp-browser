# Relay blob upload — current state

**As of:** 2026-08-24 (planning complete)

---

## Summary

| Track | Status |
|-------|--------|
| **Planning** | Done — [DESIGN.md](DESIGN.md), [DECISIONS.md](DECISIONS.md) R001–R018 |
| **www server** | Shipped — [relay blob design](../../../web2/www/Plans/2026-08-24-relay-blob-upload-design.md) |
| **pp-browser client** | **Not started** — next: **i1** shared blob client |

---

## What exists today

### www (Brief)

- `POST /api/relay/v1/blobs/presign|retain|delete|list`
- `POST /api/relay/v1/profile/icon`
- S3 presigned PUT + CDN `public_url`
- Quotas, GC, sign domains documented

### pp-browser

| Area | State |
|------|-------|
| `HttpClient` | GET + POST only — **no PUT** |
| Blob sign helpers | **Missing** |
| `IBlobClient` | **Missing** |
| Profile icon UI | **Missing** — nickname/IDs only in Me → Profile |
| Directory `icon` parse | **Missing** |
| Local icon cache | **Missing** |
| `ChatContentType::Attachment` | **Missing** |
| Composer attach | **Missing** |
| `{thread_id}/blobs/` | D075 placeholder only |
| `contact_card.avatar_url` | Encoded/decoded; **not rendered** |

---

## Next agent — start here

1. Read [DESIGN.md § 4.1](DESIGN.md#41-shared-blob-stack-i1) and www sign domain table.
2. Implement **i1** in [PHASES.md](PHASES.md#i1--shared-blob-client-foundation):
   - `HttpClient::Put`
   - `RelayBlobSignPayload` + `HttpBlobClient`
   - Unit tests against known sign byte layout
3. Do **not** start attachment wire (a1) until i1 is reviewable.

---

## Gaps / risks

| Risk | Mitigation |
|------|------------|
| Old clients hard-reject attachment type | Ship soft-skip (a1 / R018) with send |
| CDN unavailable after GC | Eager local download (R008) |
| Large upload UX | Upload-before-send + progress (R015) |
| Quota surprise | a4 confirm pop-older (relay only) |

---

## Related shipped work to reuse

- Registration sign pattern: `RegistrationSignPayload.*`, `RelaySignBytes.*`
- Nickname update HTTP flow: `HttpRegistrationClient`
- E2E tiers: [e2e-message-crypto](../e2e-message-crypto/)
- Call “encrypt once” precedent: [CALLS.md](../../docs/architecture/CALLS.md) V004
