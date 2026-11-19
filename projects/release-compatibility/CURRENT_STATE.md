# Release compatibility — current state

**As of:** 2026-11-19

Design written ([DESIGN.md](DESIGN.md)); RC001–RC016 accepted by Hongwei (2026-11-17/19), in review with Leo
(the relay's numbering, its dev tags and the environments wait for him; RC007 answers his review of the branch model; RC008 is the app build
ladder; RC009 the environments; RC010 backups; RC011 what operating a live release adds; RC012 the release checklist
and runbooks; RC013 regression suites; RC014 the backport and hotfix rules from Leo's second
review; RC015 Kenneth's client-side questions; RC016 three independent version lines).

**P0 written:** [INVENTORY.md](INVENTORY.md), at the commits of 2026-11-19: every format, what an unknown version or
field does today, and the release-1 work list (§ 4). In review with Hongwei, Kenneth (RC015's rows, the update
text) and Leo (the relay's rows). P0's decisions are taken (RC017: a clean local start, `pp_credit` fixed, the easy
recovery tier and N031 after release 1); P1, the freeze, starts once the list is agreed. Until release 1 ships, [COMPATIBILITY.md](../../docs/contracts/COMPATIBILITY.md) (clean cutovers) is the
policy in force, but new breaking changes should already be weighed against the range rule.
