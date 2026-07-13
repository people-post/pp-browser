# Push notifications — decisions

## P001 — Opaque wake only

**Date:** 2026-07-13  
**Decision:** FCM/APNs payloads carry `type=inbox_wake` (data message). No message text, contact names, or thread ids.  
**Rationale:** Aligns with E2E threat model; OS vendors and Brief must not learn content via push.

## P002 — No commercial push SaaS

**Date:** 2026-07-13  
**Decision:** Brief talks to FCM (and later APNs) directly. No OneSignal / Firebase Console product analytics as source of truth.  
**Rationale:** Aggregators see engagement stats across the user base; conflicts with privacy product stance.

## P003 — PIN-locked generic notify

**Date:** 2026-07-13  
**Decision:** Background sync must not unlock the vault. Alerts on + locked → generic “New message”. Alerts off + locked → silent.  
**Rationale:** Avoids decrypt-without-consent and PIN bypass via wake path.

## P004 — Desktop local-only v1

**Date:** 2026-07-13  
**Decision:** Desktop uses local OS notifications + in-process background poll. No remote desktop push while process is dead.  
**Rationale:** Desktop always-on wake is out of scope; process-alive UX is enough for v1.

## P005 — Alerts ≠ sync

**Date:** 2026-07-13  
**Decision:** **Show notifications** controls OS banners and FCM registration only. Inbox sync stays on via slow poll / WorkManager when alerts are off. Unregister FCM when alerts off.  
**Rationale:** Users who silence banners still want unread catch-up; unregister keeps Brief send stats honest.

## P006 — Background poll + WorkManager fallback

**Date:** 2026-07-13  
**Decision:** Implement D032 background path: in-process poll at `kBackgroundRelayPollIntervalMs` (45s) while alive; Android WorkManager ~15 min when process dead and alerts off; rarer backup (1–6h) when alerts on.  
**Rationale:** Push is wake-only; poll remains source of truth. Fast background poll is rejected for battery/Doze.

## P007 — iOS deferred

**Date:** 2026-07-13  
**Decision:** Interfaces may mention `platform: ios`; APNs implementation after Android E2E.  
**Rationale:** Android is shipped/m milestone 2; iOS is reserved.
