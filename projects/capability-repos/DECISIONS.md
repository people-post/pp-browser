# Capability repos — decisions

Living ADRs. Promote durable layout rules into [SRC_LAYOUT.md](../../docs/architecture/SRC_LAYOUT.md) when they
ship; mark superseded here.

---

## CR001 — Capability repos apps pick, not one engine repo

**Date:** 2026-10-29  
**Status:** accepted

**Context:** Messaging, calls and broadcast are about 83k of pp-browser's 179k source lines. One "engine" repo
for everything below the GUI would also carry most of `foundation`, `common`, `lib/people` and `lib/net`
(~900 include sites from the moved code), and every capability change would touch it.

**Decision:** Separate capability repos (identity, messaging, realtime, AI, node) on a thin shared base;
pp-browser and other apps pick the capabilities they need.

**Consequences:** A capability + UI change becomes a release plus a pin. Accepted, and contained by splitting
in-repo first (CR004).

## CR002 — Runtime core goes to pp-cpp-common

**Date:** 2026-10-29  
**Status:** accepted

**Context:** Every capability depends on the threading model (`AppRuntime`, owner threads, `OwnerOutbox`).
The core (~2.2k lines) already builds only on pp-cpp-common's executors, logger and metrics.

**Decision:** Move the runtime core into pp-cpp-common; app lifecycle, versions, branding and frame pacing
stay in apps. Owner threads become registered by apps and capabilities instead of a pp-browser enum.

## CR003 — Calls and broadcast are one repo ("realtime")

**Date:** 2026-10-29  
**Status:** accepted

**Context:** They share `lib/media`, have a cycle between them (broadcast → calls 7 include sites, calls → broadcast 2),
and broadcast alone is 5.6k lines.

**Decision:** One realtime repo. Revisit if broadcast grows a life of its own.

## CR004 — In-repo boundaries before repo moves; the app owns the version set

**Date:** 2026-10-29  
**Status:** accepted

**Context:** A repo split is cheap to do and costly to live with when the boundary still moves. With many
repos, two capabilities can pin two versions of a third.

**Decision:** Each capability first becomes its own target set inside pp-browser behind a gated boundary
(no new upward edges); it moves only once that has held. Capability repos pin dependencies for their own
CI only; each app keeps one version list that wins.
