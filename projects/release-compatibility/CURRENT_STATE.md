# Release compatibility — current state

**As of:** 2026-11-20

Design written ([DESIGN.md](DESIGN.md)); RC001–RC016 accepted by Hongwei (2026-11-17/19), in review with Leo
(the relay's numbering, its dev tags and the environments wait for him; RC007 answers his review of the branch model; RC008 is the app build
ladder; RC009 the environments; RC010 backups; RC011 what operating a live release adds; RC012 the release checklist
and runbooks; RC013 regression suites; RC014 the backport and hotfix rules from Leo's second
review; RC015 Kenneth's client-side questions; RC016 three independent version lines).

**P0 written:** [INVENTORY.md](INVENTORY.md), at the commits of 2026-11-19: every format, what an unknown version or
field does today, and the release-1 work list (§ 4), agreed by Hongwei on 2026-11-19. P0's decisions are taken (RC017: a clean local start, `pp_credit` fixed, the easy
recovery tier and N031 after release 1; RC018: the answers for Kenneth's and Leo's rows, the
rest theirs with defaults). **P1, the freeze, is in force since 2026-11-19:** no breaking change outside the list until
release 1 ships ([PHASES § P1](PHASES.md#p1--freeze)). **P2 started 2026-11-19** ([RC019](DECISIONS.md#rc019--p2-starts-one-main-the-relay-deploys-from-main-and-tags-no-protection-on-the-private-relay-repos)): since 2026-11-20 every repo has only `main` (`develop` deleted); the relay repos deploy qa from a push to `main` and prod from a `vX.Y.Z` tag that only Hongwei pushes. Regression suites (P2, [RC013](DECISIONS.md#rc013--regression-suites-in-tiers-picked-by-capability-run-remotely)): the app lab's scenarios are tagged by capability, `--tags` and `--changed` pick them, and lab smoke runs the `smoke` set after every merge to main ([TEST_STRATEGY § Lab smoke](../../docs/ops/TEST_STRATEGY.md#lab-smoke)); it and the nightly stay idle until the lab machine's variables and AWS role are set (O3); the relay's after-deploy smoke run is still to do. Next: the rest of P2 (branch protection, tag workflows, environments), and P3 (the list built). Until release 1 ships, [COMPATIBILITY.md](../../docs/contracts/COMPATIBILITY.md) (clean cutovers) is the
policy in force, but new breaking changes should already be weighed against the range rule.

## Open questions for Kenneth and Leo

[RC018](DECISIONS.md#rc018--the-release-1-answers-for-kenneths-and-leos-rows) part 2 holds the questions left to each of them. Every row has a default that stands until they
decide, so no work waits on them: build to the default, and change it only when they answer.

**An agent starting work for Kenneth or Leo on this project:** read RC018 (part 1 is decided; part 2 is theirs),
then [INVENTORY § 3](INVENTORY.md#3-rows-from-rc015-kenneth-and-for-leo) and the items each row names. Bring the open
rows to your person before building on them. To record an answer, edit that row in RC018 part 2 (the decision, who,
the date) and the INVENTORY item it names, in one PR, and drop the row from the lists below.

- **Kenneth** (`@gzdemon77`, client UI, texts, the AI client):
  - the update-needed display and words (INVENTORY U1, U2, U11, T14);
  - the server's `message` vs the app's translations (U12);
  - both deletion confirmations' text, and *Delete account…* offline (O5);
  - the crash-report opt-in (O6);
  - person-level preferences in the locker.
- **Leo** (`@leofreiheit`, the relay and its infrastructure):
  - the operator set: its keys, threshold and holders, and whether relay certificates expire (O4, C7);
  - where versions are counted (O1), and how `RELAY_VERSION` reaches the functions (A1);
  - deploys: tags, Lambda versions and aliases (X11);
  - checks before deploy, and the nightly lab's variables (O3);
  - prod's update links on the release day (C3);
  - Support retention and the S3 lifecycle days (O5);
  - the crash-report endpoint (O6);
  - the support-worker's "not readable" reply (X9), the wake payload version (X10), PITR on every state table.

Also unchecked, because it lives in Leo's infrastructure repo: the S3 lifecycle, TTL and backup settings of the real
tables and buckets (INVENTORY § 2.5).
