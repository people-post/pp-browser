# `feature/messaging/calls/` — call session band

**Intent:** call **session** orchestration (ring/accept/topology/media bridge), nested under
legacy `feature/messaging` until top-level `feature/calls` is unblocked.

See [F004](../../../../projects/feature-layer-reorg/DECISIONS.md#f004--calls-home-nested-band-first-then-top-level)
and [F007](../../../../projects/feature-layer-reorg/DECISIONS.md#f007--vocabulary--end-state-feature-names).

Still compiled into `pp_feature_messaging` (same CMake target). Do not add `pp_feature_calls`
until conversations hub no longer owns `CallStack` via a reverse edge through
`MeshMessagingService`.
