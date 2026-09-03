# Pricing (initiation + relay quotes)

Cross-cutting pricing slice brought forward ahead of payment rails.

| Concern | Purpose | Surface |
|---------|---------|---------|
| **Initiation floor** | Anti-abuse for opening chat/call with a peer | Directory / identity; user↔user |
| **Media relay quote** | Regulate hop capacity ecosystem | Ad-hoc mesh quote/accept |

Currency stub: **`pp_credit`** (display “Credits”), integer **minor units**. Final name/symbol TBD.

### This slice (client)

- Outbound chat/call blocked when offer `> 0` (rails unavailable); localized payment errors
- Incoming call ring: offer copy + **Accept free** / disabled **Accept & charge**
- `ConversationsHub::SendChargeRequired` re-locks a peer (`charge_required` wire + local MarkClosed)
- Media SoftMigrate: all paid hops → `call.error.payment_unavailable_media`
- Chat compose disabled when peer floor unpaid

See [DECISIONS.md](DECISIONS.md).
