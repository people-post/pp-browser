# Pricing (initiation + relay quotes)

Cross-cutting pricing slice brought forward ahead of payment rails.

| Concern | Purpose | Surface |
|---------|---------|---------|
| **Initiation floor** | Anti-abuse for opening chat/call with a peer | Directory / identity; user↔user |
| **Media relay quote** | Regulate hop capacity ecosystem | Ad-hoc mesh quote/accept |
| **Payment promise receipts** | Signed local promise/outcome before rails | `PaymentPromiseStore` / codec / avoid |

Currency stub: **`pp_credit`** (display “Credits”), integer **minor units**. Final name/symbol TBD.

### This slice (client)

- Local signed payment promise + outcome receipts (`payment_promises.json`, P002)
- Local avoid helper stamps receipt + contact `Blocked` (no public reputation yet)
- `PaymentPromiseLifecycle` create/accept/outcome/avoid; store owned by `MessagingHub`
- Wire control codec + Hub/Facade APIs for offer/accept/outcome/avoid/ingest
- Outbound chat/call blocked when offer `> 0` (rails unavailable); localized payment errors
- Incoming call ring: offer copy + **Accept free** / disabled **Accept & charge**
- `MessagingHub::SendChargeRequired` re-locks a peer (`charge_required` wire + local MarkClosed)
- Media SoftMigrate: all paid hops → `call.error.payment_unavailable_media`
- Chat compose disabled when peer floor unpaid

See [DECISIONS.md](DECISIONS.md).
