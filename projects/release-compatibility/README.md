# Release compatibility

From release 1 on, every app, pp-node and relay works with every other one whose version is inside the supported
range. Breaking changes are still allowed, but only for versions leaving the range. This project moves the product
from pre-release (clean cutovers, everyone updates together) to that rule, across pp-browser, pp-node, the pp-cpp-*
libraries and pp-relay-apis.

Start with [DESIGN.md](DESIGN.md); order of work in [PHASES.md](PHASES.md); status in [CURRENT_STATE.md](CURRENT_STATE.md); decisions in [DECISIONS.md](DECISIONS.md); P0's inventory and release-1 work list in [INVENTORY.md](INVENTORY.md); the signed formats of the release-1 cutover (versions, extension areas, algorithm names, the relay certificate) in [SIGNED_FORMATS.md](SIGNED_FORMATS.md), and how they are built and landed in [CUTOVER_PLAN.md](CUTOVER_PLAN.md).
