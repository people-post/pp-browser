# Push notifications

**Status:** Wave 1 implemented (p0–p7)  
**Owner:** Hongwei + agents  
**Stable refs:** [docs/contracts/SERVICE_ENDPOINTS.md](../../docs/contracts/SERVICE_ENDPOINTS.md), [docs/architecture/PLATFORMS.md](../../docs/architecture/PLATFORMS.md)  
**Related:** [chat-storage-and-memory](../chat-storage-and-memory/) (D032 poll), [e2e-message-crypto](../e2e-message-crypto/) (opaque wake; ciphertexts on relay)

**Cross-repo:** Brief relay device APIs + FCM send live in `web2/www` (`/api/relay/v1/devices/*`).

## One-line goal

Wake clients with opaque FCM data messages from Brief (no commercial push SaaS), show local OS alerts after client-side ingest, and keep inbox sync via slow poll / WorkManager when the user turns alerts off.

## Release scope (v1)

| In | Out |
|----|-----|
| Brief-owned FCM gateway + device register/unregister | Commercial aggregators (OneSignal, etc.) |
| Opaque `inbox_wake` payloads only | Message text / contact names on the push path |
| `Show notifications` pref (alerts ≠ sync) | Toggle to disable all background sync |
| Background poll 30–60s while process alive | Desktop remote wake when process dead |
| Android WorkManager ~15 min when alerts off | iOS / APNs implementation |
| Desktop local OS notifications while running | Rich media push previews |

## Documents

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Threat model, schedule matrix, alerts vs sync |
| [CURRENT_STATE.md](CURRENT_STATE.md) | What the codebase does today |
| [PHASES.md](PHASES.md) | Delivery checklist |
| [DECISIONS.md](DECISIONS.md) | ADRs (P001+) |

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| p0 | Project docs + ADRs | Done |
| p1 | Contracts, prefs, platform stubs | Done |
| p2 | Brief device APIs + FCM send | Done |
| p3 | BackgroundSyncScheduler + WorkManager | Done |
| p4 | Android FCM E2E | Done |
| p5 | Desktop local notifier | Done |
| p6 | Settings UI toggle | Done |
| p7 | Promote to docs/contracts | Done |
