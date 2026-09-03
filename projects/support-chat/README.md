# Support chat (customer entry)

**Status:** implementing (client entry)  
**Server design (canonical):** [`web2/www/Plans/2026-08-24-support-chat-design.md`](../../../web2/www/Plans/2026-08-24-support-chat-design.md)

## Product meaning

**PP Support** is the **app / product customer-support team** (help with PP), not a “relay support” desk. Staff answer from `support.brief.global`. Messenger/relay is only the delivery path — the same ops group may also run a relay, but that is unrelated to what the Me-tab entry means for users.

## One-line goal

Let pp-browser users open a **Support** Direct thread to the PP Support Account using existing messaging — no agent UI in the native app.

## Scope (thin)

| In | Out |
|----|-----|
| Read nested `support` from `GET …/v1/client-compat` | Staff console (app-support-static) |
| If `enabled` + `account_id`: Me-tab Support entry | New wire / thread kinds |
| Ensure Contact + `FindOrCreateDirectThread(…, E2ePublic)` | Ticket/assignment UX |
| Suppress device-lock on Support thread | Decrypt for ops (server-side in app-support) |
| Reuse send / poll / history path | `/api/support/v1` |

## Discovery shape

```json
"support": {
  "enabled": true,
  "account_id": "account:…",
  "display_name": "PP Support"
}
```

Omit or `enabled: false` → hide Support entry.

## Depends on

- www client-compat `support` field (done)
- PP Support Account provisioned
- Existing Direct `e2e_public` + ConversationsHub / InboxController

## Implementation checklist

1. [x] Extend client-compat parse/cache for `support`
2. [x] Publish Support discovery from ClientCompatController → Application
3. [x] Me tab row → ensure Contact + open/focus Support thread
4. [x] Locale strings for display name fallback
5. [x] Suppress `CanLockPublicToThisDevice` for Support account thread
6. [ ] Manual test: customer text ↔ agent reply (opaque relay transport; staging E2E)

## Manual verify

1. Compat with Support enabled → Me shows Support row  
2. Compat disabled/absent → row hidden  
3. Tap → Support chat opens; first send auto-keys; agent reply appears  
4. Thread actions menu has no “lock to this device”
