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
- Wire control codec + Hub/Facade APIs for offer/accept/outcome/avoid/stage
- **P003:** inbound receipts stage in `pending_inbound[]` (Accept/Ignore); peer-chat offer helper forces payer-ack + `thread:<id>` service_ref; receive pipeline stages (does not auto-commit)
- Outbound chat/call blocked when offer `> 0` (rails unavailable); localized payment errors
- Incoming call ring: offer copy + **Accept free** / disabled **Accept & charge**
- `MessagingHub::SendChargeRequired` re-locks a peer (`charge_required` wire + local MarkClosed)
- Media SoftMigrate: all paid hops → `call.error.payment_unavailable_media`
- Chat compose disabled when peer floor unpaid

### Canonical names (do not thrash)

Keep these spellings stable across code, docs, and agents:

| Layer | Canonical symbols |
|-------|-------------------|
| Receipt type | `PaymentPromise`; fields `promise_id`, `payer_account_id`, `payee_account_id`, `amount_minor`, `service_ref`, `release_rule`, `local_avoid`, `outcome_*` |
| Release rule | `PaymentPromiseReleaseRule::PayerAck` (default); JSON/wire `payer_ack` |
| Store | `PaymentPromiseStore`; file `payment_promises.json`; keys `promises[]`, `pending_inbound[]` |
| Store ops | `Load`/`Save`/`Upsert`/`StageInbound`/`AcceptInbound`/`IgnoreInbound`/`MarkLocalAvoid`/`HasLocalAvoidAgainst` |
| Lifecycle | `PaymentPromiseLifecycle::OfferParams`, `CreateOffer`, `Accept`, `MarkDelivering`, `RecordOutcome`, `AvoidCounterparty` |
| Avoid | `PaymentPromiseAvoid::AvoidCounterparty`, `ShouldAvoid`; contact `TrustLevel::Blocked` |
| Receipt codec | `PaymentPromiseCodec` (JSON + ML-DSA sign/verify) |
| Wire codec | `PaymentPromiseWireCodec`; controls `promise_offer` / `promise_accept` / `promise_outcome` |
| Hub/Facade | `CreatePaymentPromiseOffer`, `CreatePaymentPromiseOfferForThread` (`service_ref=thread:<id>`), `StagePaymentPromiseControlMessage`, `AcceptInboundPaymentPromise`, `IgnoreInboundPaymentPromise`, … |
| Receive path | `RelayReceivePipeline::ApplyInboundPaymentPromiseMessage` → `StageInbound` only |

`Stage*` means pending only; `AcceptInbound*` commits; `IgnoreInbound*` drops. Do not revive `IngestPaymentPromise*` — it was renamed to `StagePaymentPromiseControlMessage`.

See [DECISIONS.md](DECISIONS.md).
