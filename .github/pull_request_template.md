## Summary

<!-- What changed and why, user-visible behaviour first. Link the issue, the decision (projects/*/DECISIONS.md) or the doc. -->
<!-- A release branch (release/app-X.Y, release/node-X.Y) takes fixes only, and only once they are on main: merge the fix here, then label this PR `backport release/app-X.Y` (one label per branch). A fix only a release branch needs: PR it to the branch with the label `release-branch-only` and a line "Why not on main: …" (docs/ops/RELEASE.md § Release branches and backports). -->

## Contracts and docs

<!-- Wire / disk / HTTP format changed? Additive or breaking (docs/contracts/COMPATIBILITY.md). Until release 1 ships, a breaking change names its item in projects/release-compatibility/INVENTORY.md § 4 (the freeze). The docs/ files updated in this PR, and decisions whose Status line changed. "None" when nothing. -->

## Depends on

<!-- PRs or deploys that must land first (relay / web2, pp-cpp-* pins) and the merge order. Follow-ups this PR leaves open. "None" when nothing. -->

## Testing

<!-- What ran and its result — counts, not "all good". New tests: the behaviour each one guards. Lab scenarios run (`--only` names). A UI change: a screenshot. -->

## Not verified

<!-- What was not run or not seen, and why: platforms CI only compiles (iOS) or this machine cannot build, device-only paths (pickers, camera, push, keychain), flows not reachable headless. What a reviewer or tester should check by hand. "None" when everything was run. -->

## Where to look

<!-- The parts that deserve a careful read: core flows touched (unlock, identity, messaging, calls), the security surface, threading. Optional for a small PR. -->
