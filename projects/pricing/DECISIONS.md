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
