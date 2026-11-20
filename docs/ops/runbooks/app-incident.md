# Runbook: app incident

**Tier:** ops

A bad app version in the field: a crash, messaging, calls or registration broken, data or keys at risk. A version
cannot be taken back from phones, so the work is to stop it spreading, ship the fix, and get users onto it.

- **When:** a released app version (store, TestFlight external testers, or a GitHub release download) is broken
  for its users; or a store rejects or delays a fix mid-rollout ([§ 5](#5-a-store-rejects-or-delays-the-fix)).
- **Not for:** a bad relay release: the relay rollback, Leo's, in pp-relay-apis (not yet written,
  [README](README.md)). A bad pp-node release takes the same steps with its tags; the fleet is Leo's (not yet: the
  pp-node fleet rollback runbook).
- **Who decides:** the release owner. Tell Hongwei and Leo at once for S1, and the team.
- **Record:** an `incident` issue, ticked as you go.

## 1. Open the incident issue

[New issue → Incident](https://github.com/people-post/pp-browser/issues/new?template=incident.md) (label `incident`),
titled `Incident: <what broke>`. Fill in as you go: severity, affected versions, the runbook (this page), who decided,
who was told, the timeline in UTC with each workflow run linked.

**Severity** (from the template):

| | Meaning | Response |
|-|---------|----------|
| **S1** | Messaging, calls or registration down, or data or keys at risk | Stop the bleed now; Hongwei and Leo told now; hotfix with expedited review |
| **S2** | A feature broken for many | Stop the bleed; hotfix in the next patch |
| **S3** | Degraded, a workaround exists | Hotfix in the next patch; the rollout may continue |

## 2. Find the affected versions

Every build names itself ([BUILD.md § Build identity](../BUILD.md#build-identity-commit-and-tag)): About shows
`Build v1.0.0-rc.3 · a1b2c3d4`; the first line of a Send logs archive and the crash record give
`1.0.0 · v1.0.0-rc.3 · a1b2c3d4`. A store build shows the tag of the candidate it was promoted from.

```bash
git fetch --tags origin
git tag --points-at 'v1.0.0-rc.3^{commit}'          # the release it became (v1.0.0), and rc-build/<n>
git show --no-patch rc-build/152                    # a TestFlight build number: rc-build/<n> or internal/<n>
git tag --contains <commit that brought the bug> --list 'v*'
git log --oneline --grep 'cherry picked from commit <sha>' origin/release/app-1.0
```

Write each affected product, version and platform in the issue, and the in-range versions not affected. Not yet:
version counts by version and OS at the relay (INVENTORY O1), to say how many users each affects.

## 3. Stop the bleed

| Where | Who | How | Verify |
|-------|-----|-----|--------|
| App Store, phased release running | release owner | App Store Connect → App Store → the version → Phased Release → **Pause**. Pauses add up to 30 days at most; users can still update by hand | The version shows the phased release paused |
| App Store, not yet released | release owner | Do not release it: App Store Connect → the version → remove it from review, or leave it waiting for a manual release | The version's status |
| TestFlight external testers | release owner | TestFlight → the build → **Expire Build** | Testers no longer get it |
| Desktop and Android APK (GitHub release) | release owner | `gh release edit <last good vX.Y.Z> --latest`; a warning line at the top of the bad release's notes (`gh release view <tag> --json body -q .body > notes.md`, add the line, `gh release edit <tag> --notes-file notes.md`). Never delete a release tag or its files | The Releases page leads with the last good one |
| Relay-side mitigation (the relay refusing or working around what the bad version sends) | Leo writes it; Hongwei tags the relay release | [release.md § 1](release.md#1-before-the-cut)'s relay step | Its `deploy` run passed; the symptom stops |

Not yet: remote switches, to turn a feature off in the field without a release (`switches` in `client-compat`,
INVENTORY O2); Play staged rollout (no Play upload yet).

**Blocking the bad version** with the update screen (Leo raises `min_client_version` above it) is a last resort,
for S1 only, and only once the fixed version is available on every platform the floor covers: it strands everyone who
cannot update yet. Hongwei decides. Not yet: the range exit runbook (PHASES § P5).

## 4. Fix it

Through the [hotfix runbook](hotfix.md): main first, the backport to each affected branch, a patch candidate
`vX.Y.Z+1-rc.1`, the patch release. S1 and security: straight from internal testers to App Store review with an
expedited review ([hotfix.md § 7](hotfix.md#7-security-fixes)). A paused phased release ends when the fixed version
is released; resume it only if the cause turned out not to be the app.

## 5. A store rejects or delays the fix

The relay never needs the new app, so a delay breaks nothing new; it only keeps the bad version out longer.

- Keep the bad version's phased release paused (step 3), and any relay-side mitigation in place.
- Answer the rejection in App Store Connect (App Review's message on the version), fix what it names, and resubmit;
  ask for an expedited review again.
- A rejection you dispute: appeal to the App Review Board ([Contact App Review](https://developer.apple.com/contact/app-store/?topic=appeal)).

## 6. Communicate

| Who | How |
|-----|-----|
| Users | Leo raises `latest_client_version` to the fixed version and may set `message`: every app below it shows the update banner with that text ([SERVICE_ENDPOINTS § Client compatibility](../../contracts/SERVICE_ENDPOINTS.md#client-compatibility-discovery)). Verify: `curl -s https://api.pp.peoplepost.org/api/relay/v1/client-compat \| jq`. The store's "What's New" for the fixed version |
| Testers | TestFlight "What to Test" on the fixed build; the testers' channel |
| The team | The incident issue, linked in the team channel |

## 7. Follow up

- The cause written in the issue.
- The fix on main and on every affected release branch (the PRs linked).
- The cheapest test that would have caught it, in a PR ([TESTING § When a higher tier finds a bug](../../architecture/TESTING.md#when-a-higher-tier-finds-a-bug)).
- S1 and S2: a postmortem in the issue: what to change so it does not recur.
- The issue closed once the fixed version is fully rolled out.
