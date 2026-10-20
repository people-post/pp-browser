# `feature/calls/` — call session module

**Intent:** call **session** orchestration (ring / accept / topology / media bridge).

See [F004](../../../../projects/feature-layer-reorg/DECISIONS.md#f004--calls-home-nested-band-first-then-top-level)
and [F007](../../../../projects/feature-layer-reorg/DECISIONS.md#f007--vocabulary--end-state-feature-names).

Built as `pp_feature_calls`. Calls and conversations do not include each other: `feature/product`
(`CallsParticipant`) owns the `CallStack` and wires it to the hub as a `HubParticipant`. Delivery goes
through `CallDeliveryPorts`; inbound control, its display and the live-call hints through the `call`
system-control family (`CallControlFamily.*`) bound in the hub's `SystemControlRegistry`.
