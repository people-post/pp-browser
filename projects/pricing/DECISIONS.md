# Pricing — decisions

## P001 — Initiation floor + media quote gates (no payment rails yet)

**Date:** 2026-08-08  
**Status:** Accepted (first code slice)

### Decision

1. **Protocol amounts** use one fixed currency stub (`pp_credit` / “Credits”) in **integer minor units**. Name/symbol may change later without reshaping amounts.
2. **Numeric amount is authoritative.** `volunteer` / `paid` are UX labels only (`amount == 0` ↔ free). Do not branch protocol on mode strings.
3. **Initiation (user↔user, anti-abuse):** one public **floor** covers opening a chat thread **or** call session. Default / missing directory field → `0`. Initiator offers `offer_amount >= floor` (may tip up) before first message / dial. Recipient or path that actually charges **rejects** offers below floor. Accept UI: show amount; only **waive (0)** or **take all** (no custom edit). Waive → full cancel/refund conceptually. After open → no further initiation fee until re-locked.
4. **`charge_required` control message** re-locks a peer relationship to closed until a new offer/accept.
5. **Outbound block:** when payable amount `> 0` and rails are unavailable, block outbound initiate/dial; confirm UX shows a **disabled** pay control with reason; offer alternatives when present.
6. **Media relay quotes:** `rate == 0` → accept/attach without payment confirm. `rate > 0` → same confirm/disabled-pay pattern; SoftMigrate may try other hops.
7. **Dogfood:** `initiation_floor` on `AppConfig` + `LocalIdentity` (no Me UI yet). Register/renew may publish floor; older servers that omit it → treat as `0`.
8. **Message-relay server surcharge** is a separate layer; this slice is user↔user (+ mesh hop quotes) only.
9. **Settle paths** (relay escrow vs on-chain submit by recipient) are specified for later; not implemented here.

### Rationale

Payment is not ready, but gates and wire must exist so volunteer/`0` and paid paths do not fork later. Initiation pricing prevents abuse; hop quotes regulate capacity — different products, shared currency stub.

### Alternatives rejected

- Special-case `mode == volunteer` in protocol (rejected — use amount).
- Per-message metering for contacts (rejected — one-shot initiation).
- Directory listing of relay rates (rejected — ad-hoc quote).
- Settings UI for floor in this slice (deferred — dogfood config knob).

### Cross-links

- Mesh N010 / N017 / N019 / N020; calls V022 / V023  
- [SERVICE_ENDPOINTS.md](../../docs/contracts/SERVICE_ENDPOINTS.md)  
- [NETWORKING.md](../../docs/architecture/NETWORKING.md) settlement note  


## P002 — Local signed payment promise receipts + avoid

**Date:** 2026-09-04  
**Status:** Accepted (foundation slice)

### Decision

1. **Early artifact** for payment-before-service is a **local signed `PaymentPromise` receipt** (promise + terminal outcome), not escrow rails and not on-chain service validation.
2. **Canonical ML-DSA-65 signatures** cover promise fields and outcome fields separately; `local_avoid` is never part of signed bytes.
3. Persist under profile `payment_promises.json` via `PaymentPromiseStore`. Export/share of receipts can come later.
4. **Local avoid:** `PaymentPromiseAvoid` stamps `local_avoid` on the receipt and best-effort sets matching contact `TrustLevel::Blocked`. Humans exit bad counterparties; software records verifiable facts.
5. Settlement (Brief escrow / multi-sig / chain) and public reputation stay out of scope for this slice.
6. `MessagingHub` owns `PaymentPromiseStore` (load with profile). `PaymentPromiseLifecycle` signs with the unlocked account ML-DSA key.
7. `PaymentPromiseWireCodec` packs signed receipts into system-message controls (`promise_offer` / `promise_accept` / `promise_outcome`). Hub/Facade expose create/accept/outcome/avoid/ingest; receive-pipeline auto-ingest and UI remain follow-ups.

### Rationale

Schema + durable signed lifecycle is the path-dependent foundation for release UI, evidence export, and later money movement. Avoid is the cheap safety valve.

### Cross-links

- Pricing P001 gates; NETWORKING settlement note; mesh N020 receipts/reputation (later)
