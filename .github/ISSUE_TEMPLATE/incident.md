---
name: Incident
about: The record of a rollback, restore, key compromise or bad version in the field, ticked while the runbook's steps are done
title: "Incident: <what broke>"
labels: incident
---

<!-- One issue per use of a runbook (projects/release-compatibility/DESIGN.md § Runbooks): relay rollback, app incident,
restore from backup, key compromise, hotfix. Runbooks: docs/ops/runbooks/ here, pp-relay-apis for the relay's. -->

- **Severity:** <!-- S1: messaging, calls or registration down, or data or keys at risk · S2: a feature broken for many · S3: degraded, a workaround exists -->
- **Affected versions:** <!-- product and version per platform (app 1.0.2 iOS, pp-node 10.0.1, relay 1.0.3); the in-range versions not affected -->
- **Runbook:** <!-- link the runbook page --> · **Decided by:** @ · **Told:** <!-- who was told, when -->

### Timeline (UTC)
- <!-- HH:MM — detected, and how (alarm, crash reports, a user) -->
- <!-- HH:MM — each decision and action, with its workflow run -->
- <!-- HH:MM — verified -->

### Runbook steps
- [ ] Before acting: the backup taken (relay rollback, restore); the version counts affected checked
- [ ] Steps done, each workflow run linked
- [ ] Verified: alarms quiet, the smoke run passing, `client-compat` answering the expected floor
- [ ] Communicated: users (the `client-compat` message, store notes), testers, the team

### Follow up
- [ ] Cause found and written here
- [ ] The fix on main, and backported to every affected release branch (link the PRs)
- [ ] The finding turned into the cheapest test that would have caught it (docs/architecture/TESTING.md § When a higher tier finds a bug): link the PR
- [ ] Postmortem (S1, S2): what to change so it does not recur
