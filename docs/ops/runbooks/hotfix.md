# Runbook: hotfix

**Tier:** ops

A fix that one or more live release branches need: it lands on main, is backported by the bot, and ships as a patch
candidate from each affected branch. The rules: [RELEASE.md § Release branches and backports](../RELEASE.md#release-branches-and-backports)
([RC014](../../../projects/release-compatibility/DECISIONS.md#rc014--backport-and-hotfix-rules-after-leos-second-review)).
Examples use `release/app-1.0`; pp-node is the same with `release/node-X.Y` and `pp-node/v…` tags.

- **When:** a bug in a release in the field, or in a candidate not yet released. The newest release branch takes
  every fix; older branches still in range take only security fixes and fixes for a break between in-range
  versions. Never features or format changes (an additive change only where the fix needs it).
- **Who:** the fix's author writes and merges it on main; the release owner approves the backport PR, merges it,
  and tags the patch.
- **Record:** a comment on the branch's release issue ("Release app 1.0") per patch; the `incident` issue too when
  the fix answers one ([app-incident.md](app-incident.md)).

## 1. Which branches

The live branches: `git fetch origin && git branch -r --list 'origin/release/*'`. A branch needs the fix when it holds
the bug:

```bash
git tag --contains <commit that brought the bug> --list 'v*'                       # releases built on it
git log --oneline --grep 'cherry picked from commit <sha>' origin/release/app-1.0   # backported to a branch
```

## 2. Fix on main first

A PR to main as usual: review, CI, merge. An urgent fix is merged on main first too; only a fix main does not need
at all is the [exception](#5-the-release-branch-only-exception).

## 3. The backport

Label the merged PR once per branch that needs it (before or after the merge):

```bash
gh pr edit <main PR> --add-label 'backport release/app-1.0'
```

[`backport.yml`](../../../.github/workflows/backport.yml) cherry-picks the PR's commits with `-x` onto
`backport/pr-<N>-to-release-app-1.0` and opens a PR to `release/app-1.0` that links the original, asking the release
owner for review. Verify: the bot's comment on the original PR ("opened #M"), or
`gh run list --workflow backport.yml --limit 1` and `gh pr list --base release/app-1.0`.

**Its checks.** A PR the bot opens starts no `pull_request` run, so the bot dispatches `build.yml` on its branch
(every platform, every test):

```bash
gh run list --workflow build.yml --branch backport/pr-<N>-to-release-app-1.0 --limit 1
```

The provenance check ([`release-branch-pr.yml`](../../../.github/workflows/release-branch-pr.yml): every commit
carries a cherry-pick trailer naming a commit on main) runs only on a `pull_request` event: close and reopen the PR
(`gh pr close <M> && gh pr reopen <M>`) to run it and the PR's build checks. For a fix in a risky area, also run the
lab scenarios it touches (`PP_APP_LAB_ARGS="--only <scenario>" ./scripts/test/pp_local_test.sh run --suite app`).
Not yet: the frozen-data tests (PHASES § P3).

The release owner approves and merges (`gh pr review <M> --approve && gh pr merge <M> --merge`). Not yet: an approval
enforced by protection on release branches (PHASES § P2).

**It does not apply.** The bot pushes nothing and comments on the original PR with the conflicting commit, its files
and the commands to do it by hand. The author does it: resolve, and say what differs on a line
`branch-specific backport: …` in that commit's message (or in a separate commit when the rest picks cleanly); open
the PR to `release/app-1.0` linking the original. The same by hand for a backport touching `.github/workflows/`
(the bot's token cannot push it).

## 4. An older live branch

A fix for app 1.0 while 1.1 is current goes to `release/app-1.0`: label `backport release/app-1.0` (and each newer
branch that needs it), tag the patch there (step 6). Its GitHub release is not marked latest, since a newer
`vX.Y.Z` exists.

## 5. The `release-branch-only` exception

A fix only a release branch needs (the code is gone from main, say): a PR straight to the branch, with the label and
the reason on a line `Why not on main: …`.

```bash
git switch -c fix/<name> origin/release/app-1.0
# commit the fix
git push -u origin fix/<name>
gh pr create --base release/app-1.0 --label release-branch-only \
  --title '[release/app-1.0] <fix>' --body $'<what and why>\n\nWhy not on main: <the reason>'
```

`release-branch-pr.yml` fails the PR without the reason (at least a few words).

## 6. The patch candidate

Once every fix is merged on the branch, tag the next patch's first candidate on its tip and follow
[release.md § 3](release.md#3-release-candidates) to [§ 7](release.md#7-phased-release), shortened: no cut, the
same release issue.

```bash
git fetch origin && git checkout --detach origin/release/app-1.0
git tag -a v1.0.1-rc.1 -m "pp-browser 1.0.1 rc.1"
git push origin v1.0.1-rc.1
```

The guard refuses `v1.0.0-rc.<n>` once `v1.0.0` exists: a fix to a release is always `vX.Y.Z+1-rc.1`. Then
`v1.0.1` on the commit of the candidate that passed, and the store submission as for a release. When the fix answers
a field problem, Leo raises `latest_client_version` to the patch so users below it see the update banner
([app-incident.md § 6](app-incident.md#6-communicate)).

## 7. Security fixes

- A security fix may go from internal testers straight to App Store review, skipping external testers.
- Ask Apple for an expedited review when submitting ([Request an expedited app review](https://developer.apple.com/contact/app-store/?topic=expedite)),
  naming the vulnerability's impact, not its details.
- The repo is public: a fix merged on main is visible before any store ships it.
- Every in-range branch that holds the bug gets the fix. Not yet: the security release runbook (an embargo until
  every line can publish together, PHASES § P5); until it exists, Hongwei decides the order of merges, tags and
  submissions.
