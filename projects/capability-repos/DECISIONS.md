# Capability repos — decisions

Living ADRs. Promote durable layout rules into [SRC_LAYOUT.md](../../docs/architecture/SRC_LAYOUT.md) when they
ship; mark superseded here.

---

## CR001 — Capability repos apps pick, not one engine repo

**Date:** 2026-10-29  
**Status:** accepted; amended by CR005 (move only for a second app) and CR006 (one capabilities repo, a target
per capability, not a repo each); its list amended by CR007 (AI stays in the app) and CR008 / CR009

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
**Status:** accepted; amended by CR006 — "realtime" is the calls and broadcast targets of the one capabilities
repo, still with no cycle between them

**Context:** They share `lib/media`, have a cycle between them (broadcast → calls 7 include sites, calls → broadcast 2),
and broadcast alone is 5.6k lines.

**Decision:** One realtime repo. Revisit if broadcast grows a life of its own.

## CR004 — In-repo boundaries before repo moves; the app owns the version set

**Date:** 2026-10-29  
**Status:** accepted; amended by CR005 (a boundary that has held moves only when a second app needs it)

**Context:** A repo split is cheap to do and costly to live with when the boundary still moves. With many
repos, two capabilities can pin two versions of a third.

**Decision:** Each capability first becomes its own target set inside pp-browser behind a gated boundary
(no new upward edges); it moves only once that has held. Capability repos pin dependencies for their own
CI only; each app keeps one version list that wins.

## CR005 — Move a capability only when a second app needs it

**Date:** 2026-10-30  
**Status:** accepted

**Context:** On 2026-10-30 no other app uses any pp-browser capability: the ledger apps pick pp-cpp-common,
-crypto and -ui, pp-ledger picks pp-cpp-amp, and the VPN client does not exist yet. A repo move makes every
later change that crosses it a release plus a pin.

**Decision:** The in-repo work (P0–P3: the gate, the cuts, a thin base, a target set per capability) goes
ahead now. A capability leaves pp-browser only when a second app needs it. pp-node is the first such app: it
already ships on its own.

**Consequences:** P4 is demand-driven, not a schedule. Until then the targets and the gate give the same
boundaries at no release cost.

## CR006 — One capabilities repo, a target per capability

**Date:** 2026-10-30  
**Status:** accepted; supersedes the repo-per-capability shape of CR001

**Context:** CR001 rejected one engine *target* that every app links whole. Separate repos per capability
fix that but cost one pin each, and two capabilities can pin two versions of a third.

**Decision:** Capabilities that leave pp-browser go to one repo, with a CMake target set per capability (and
options so a consumer builds only what it picks). An app fetches it once and links the targets it needs.

**Consequences:** One pin per app instead of five, and no version skew between capabilities. A consumer
fetches source it does not build. The capability order (DESIGN § Target) is enforced inside that repo by the
same gate.

## CR007 — AI stays in the app

**Date:** 2026-10-30  
**Status:** accepted

**Context:** The planner, prompts and RML generation (`lib/ai`, `feature/ai`) are bound to the shell's own UI
rules (`PromptBuilder`, the RML and RCSS profiles). Another app would not want them.

**Decision:** AI is part of the app, not a capability. Capabilities that need AI (messaging tools, structured
text) declare ports the app implements. Only generic tool / MCP plumbing may leave later, if an app asks.

## CR008 — Identity is people; the backend client is net; pricing is part of messaging

**Date:** 2026-10-30  
**Status:** accepted; amends DESIGN's identity (people + net + pricing)

**Context:** The planned identity repo held three different things. People (keys, cards, contacts, devices,
the locker format) is identity. Net is the Brief backend client (registration, directory, blobs, push):
deployment-specific product code, against rule 1. Pricing is policy used by messaging (16 include sites)
and calls (5), and it needs messaging's thread and control types.

**Decision:** Identity = people (with `common/directory`). Net is its own capability above people. Pricing is
part of messaging.

## CR009 — Posts and content are one capability, below messaging

**Date:** 2026-10-30  
**Status:** accepted

**Context:** `lib/posts` depends only on crypto; `lib/content` (the content store) is used by posts and by
messaging's attachments. Neither had a place in the plan.

**Decision:** "Social" = posts + content, above people and below messaging. Posts never depends on messaging
(social-posts S029 keeps notices off the messaging path).

## CR010 — Wire contracts and vectors in their own repo

**Date:** 2026-10-30  
**Status:** accepted; not started

**Context:** Formats now have a C++ and a TypeScript implementation each (posts in app-posts, the locker and
the KEM binding in app-static-api), and vectors are copied by hand ("copy the file again when pp-browser
regenerates it").

**Decision:** Specs and golden vectors for formats implemented in more than one language move to their own
repo, which both sides pin and test against. Independent of capability moves.

## CR011 — Layers inside, capabilities across; the gate covers tests

**Date:** 2026-10-30  
**Status:** accepted

**Decision:** Both axes hold. Layers ([SRC_LAYOUT](../../docs/architecture/SRC_LAYOUT.md)) still order code
inside every capability, and a capability's contract DTOs under `common/<capability>/` belong to it
(`common/chat`, `common/thread` → messaging; `common/directory` → people). Across capabilities the order of
DESIGN § Target holds, checked by `scripts/check/check_capability_edges.py`, tests included: a capability's
tests move with it.

## CR012 — Before any repo moves: pins, vendored deps, visibility

**Date:** 2026-10-30  
**Status:** accepted

**Decision:**
- **Pin tooling first:** one pin list per app and a local mode that builds sibling checkouts together exist
  before the capabilities repo does (pp-ledger-apps' `PpFetchPin.cmake` is the starting point).
- **One home for vendored third-party code:** it stays committed (never fetched at configure), in one shared
  place rather than a copy per repo.
- **Public under people-post**, like the pp-cpp-* repos.
- The runtime core can go to pp-cpp-common any time (P2); the hard lab and pp-node packaging go with node.

