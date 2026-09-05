# `feature/calls/` — call session module

**Intent:** call **session** orchestration (ring / accept / topology / media bridge).

See [F004](../../../../projects/feature-layer-reorg/DECISIONS.md#f004--calls-home-nested-band-first-then-top-level)
and [F007](../../../../projects/feature-layer-reorg/DECISIONS.md#f007--vocabulary--end-state-feature-names).

Built as `pp_feature_calls`. Conversations depends on calls (not the reverse):
delivery goes through `CallDeliveryPorts`; inbound control through `CallControlInboundPorts`.

`ConversationsHub` still owns `CallStack` for now; app-only ownership is a later peel.
