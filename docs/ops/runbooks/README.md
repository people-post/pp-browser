# Runbooks

**Tier:** ops

One page per operational action, each with the same shape: when to use it, who decides, what to do before acting,
the steps (each a workflow run or one command, and how to check it), how to communicate, and the follow-up
([release-compatibility DESIGN § Runbooks](../../../projects/release-compatibility/DESIGN.md#runbooks),
[RC012](../../../projects/release-compatibility/DECISIONS.md#rc012--a-runbook-for-every-operational-action-each-use-recorded)).
Each use leaves a record: the `release` issue for a release, an `incident` issue
([template](../../../.github/ISSUE_TEMPLATE/incident.md)) for everything else, ticked while the steps are done.

| Runbook | Use it when | Not for | Record |
|---------|-------------|---------|--------|
| [release.md](release.md) | Shipping a new app (`release/app-X.Y`) or pp-node (`release/node-X.Y`) release: the cut, candidates, go / no-go, the final tag, the store | A fix to a release already cut: [hotfix.md](hotfix.md) | The `release` issue opened when the branch is cut |
| [hotfix.md](hotfix.md) | A fix one or more live release branches need: the backport, then a patch candidate `vX.Y.Z+1-rc.1` | Features and format changes (release branches take neither) | The branch's `release` issue; an `incident` issue when the fix answers one |
| [app-incident.md](app-incident.md) | A bad app version on phones or desktops; a store rejecting or delaying a fix mid-rollout | A bad relay release (the relay rollback, below) | An `incident` issue |

**The relay's runbooks** (relay rollback, restore from backup, key compromise) live in
[pp-relay-apis](https://github.com/people-post/pp-relay-apis) and are Leo's. Not yet: written
([PHASES § P2](../../../projects/release-compatibility/PHASES.md#p2--one-main-release-branches)). Until they are, a bad
relay release goes to Leo and Hongwei directly, recorded in an `incident` issue here.

**Later runbooks.** Not yet: range exit and security release, before the first compatibility window closes
([PHASES § P5](../../../projects/release-compatibility/PHASES.md#p5--release-mode-in-force)); key rotation, the
pp-node fleet rollback and environment resets after that (Leo's).

## Who

| Role | Who | Does |
|------|-----|------|
| **Release owner** | Hongwei for release 1 (app 1.0, pp-node 10.0, relay 1.0); rotates after it | Cuts release branches, pushes release tags, approves backports, decides go / no-go and incidents. The backport bot asks the repo variable `RELEASE_OWNER` for review |
| **Relay prod deployer** | Hongwei (pp-relay-apis variable `RELAY_PROD_DEPLOYERS`) | Pushes the relay's `vX.Y.Z` tags, the only way to deploy relay prod |
| **Relay settings and infrastructure** | Leo (`@leofreiheit`) | Prod `client-compat` values (`min_client_version`, `latest_client_version`, `upgrade_url`, `upgrade_urls`, `message`), alarms, backups, the pp-node fleet |

## Before using one

The commands assume a pp-browser checkout with every tag (`git fetch --tags origin`) and `gh` signed in with write
access to `people-post/pp-browser`. Release mechanics (tag forms, what each workflow does, artifacts):
[RELEASE.md](../RELEASE.md).
