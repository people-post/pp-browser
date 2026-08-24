# Support chat (customer entry)

**Status:** planning (docs only)  
**Server design (canonical):** [`web2/www/Plans/2026-08-24-support-chat-design.md`](../../../web2/www/Plans/2026-08-24-support-chat-design.md)

## One-line goal

Let pp-browser users open a **Support** Direct thread to Brief’s support Account using existing messaging — no agent UI in the native app.

## Scope (thin)

| In | Out |
|----|-----|
| Read nested `support` from `GET …/v1/client-compat` | Staff console (app-support-static) |
| If `enabled` + `account_id`: pinned Support entry | New wire / thread kinds |
| `FindOrCreateDirectThread(account_id, E2ePublic)` | Ticket/assignment UX |
| Reuse send / poll / history / attachments path | Decrypt for ops (server-side in app-support) |

## Discovery shape

```json
"support": {
  "enabled": true,
  "account_id": "account:…",
  "display_name": "Brief Support"
}
```

Omit or `enabled: false` → hide Support entry.

## Depends on

- www client-compat extension (implementation)
- Brief Support Account provisioned (Slice 2)
- Existing Direct `e2e_public` + MessagingHub / InboxController

## Later implementation checklist

1. Extend client-compat parse/cache for `support`
2. Shell / Me entry → open or focus Support thread
3. Locale strings for display name fallback
4. Manual test: customer text ↔ agent reply via relay
