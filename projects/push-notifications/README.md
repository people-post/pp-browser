# Push notifications

**Status:** Wave 1 implemented (p0–p7)  
**Owner:** Hongwei + agents  
**Stable refs:** [docs/contracts/SERVICE_ENDPOINTS.md](../../docs/contracts/SERVICE_ENDPOINTS.md), [docs/architecture/PLATFORMS.md](../../docs/architecture/PLATFORMS.md)  
**Related:** [chat-storage-and-memory](../chat-storage-and-memory/) (D032 poll), [e2e-message-crypto](../e2e-message-crypto/) (opaque wake; ciphertexts on relay), [p2p-av-calls](../p2p-av-calls/) (`call_wake` planned for a1), [multi-device-account](../multi-device-account/) (per-device push under account / `relay:` binding)

Relay providers that speak this HTTP profile implement device register/unregister and optional FCM/APNs wakes; pp-browser does not depend on a specific server codebase.

## One-line goal

Wake clients with opaque push from the configured relay (FCM Android / APNs iOS; no commercial push SaaS), show local OS alerts after client-side ingest, and keep inbox sync via slow poll / WorkManager when the user turns alerts off.

## Release scope (v1)

| In | Out |
|----|-----|
| Relay-owned FCM + APNs gateway + device register/unregister | Commercial aggregators (OneSignal, etc.) |
| Opaque `inbox_wake` / `call_wake` payloads only | Message text / contact names on the push path |
| `Show notifications` pref (alerts ≠ sync) | Toggle to disable all background sync |
| Background poll 30–60s while process alive | Desktop remote wake when process dead |
| Android WorkManager ~15 min when alerts off | Rich media push previews |
| iOS APNs silent wake + token register | |
| Desktop local OS notifications while running | |

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
| p2 | Relay device APIs + FCM send (provider) | Done |
| p3 | BackgroundSyncScheduler + WorkManager | Done |
| p4 | Android FCM E2E | Done |
| p5 | Desktop local notifier | Done |
| p6 | Settings UI toggle | Done |
| p7 | Promote to docs/contracts | Done |
