---
name: Release
about: One per release, opened when its release branch is cut; the checklist and the go / no-go record
title: "Release <product> X.Y"
labels: release
---

<!-- Opened by .github/workflows/release-issue.yml when release/app-X.Y or release/node-X.Y is cut; by hand, fill the
<placeholders>. Checklist: projects/release-compatibility/DESIGN.md § Release checklist. Link what CI and the workflows
check (runs, the staging deploy) rather than re-checking it; strike what this product does not ship. The relay's list
is Leo's, in pp-relay-apis: link it. -->

- **Branch:** `<branch>` · **Tags:** `<tag>X.Y.Z`, candidates `<tag>X.Y.Z-rc.N` · **Release owner:** @<owner>
- **Go / no-go:** pending <!-- go or no-go, the date, who decided, why -->
- **Backports to this branch:** label the merged main PR `backport <branch>` (docs/ops/RELEASE.md § Release branches and backports)

### Before the cut
- [ ] Format changes since the last release listed, each with its class and range-rule step; the version table updated
- [ ] Any relay release this one needs: deployed to prod first
- [ ] Floor raises or windows closing in this release: announced weeks earlier (the update banner)
- [ ] Freeze announced; release owner named

### Release candidate
- [ ] `<tag>X.Y.Z-rc.N` tagged on `<branch>`; artifacts built once (iOS, Android, desktop, pp-node image)
- [ ] Full CI on every shipped platform, frozen-data tests and the app ↔ relay behaviour suite green
- [ ] Staging: the relay candidate deployed; the store app (N−1) and the candidate tested against it; relay rollback rehearsed
- [ ] Lab scenarios the release touches, and the latest nightly lab, green
- [ ] Real devices from internal testing: register, message, call, push and VoIP wake, link a device, account deletion
- [ ] Release notes and TestFlight "What to Test" written

### Go / no-go
- [ ] No open blocker; known issues listed
- [ ] Version counts reviewed: who is on what, and who an exit affects
- [ ] Rollback path written: the relay's previous tag, the switches that turn this release's features off
- [ ] Relay backup taken before the release

### Rollout
- [ ] Relay: canary 10% for 30 minutes, alarms quiet, then everyone
- [ ] App: external testers, then the store through the promotion workflow (tags `vX.Y.Z`), phased release; crash reports and error-code alarms watched
- [ ] pp-node: image published; fleet rolled out (ansible, Leo); owners of user-run nodes told

### After
- [ ] `client-compat`'s `latest_client_version` raised; the next floor raise scheduled
- [ ] The release branch's open period and the version table updated
- [ ] Anything went wrong: an incident issue, its finding turned into a cheaper test
