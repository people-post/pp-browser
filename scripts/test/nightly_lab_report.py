#!/usr/bin/env python3
"""Nightly lab report: one GitHub issue per failing scenario, kept up to date.

Reads the summaries pp_app_lab.py --summary wrote (one per suite), then with the `gh` CLI:
  - a scenario that failed (twice: --retry-failed) gets an issue "Nightly lab: <suite>/<scenario> fails"
    (label nightly-lab), or a comment on the one already open;
  - a scenario with an open issue that passed (or only flaked) gets a comment and the issue closed;
  - a suite whose summary is missing (the lab did not finish) is reported like a failing scenario.
Flakes are listed in the run's step summary only. Issue text is the scenario, the first line of its
error, the commit and the run link — nothing more (the logs stay in the run's artifacts).

  nightly_lab_report.py --summary app=app.json --summary app-nat=app-nat.json \\
      --sha <commit> --run-url <url> [--dry-run]
"""

import argparse
import json
import os
import subprocess
import sys

LABEL = "nightly-lab"


def gh(*args, dry_run=False):
    if dry_run and args[0] in ("issue", "label") and args[1] in ("create", "comment", "close"):
        print("dry-run: gh %s" % " ".join(args))
        return ""
    return subprocess.run(["gh", *args], check=True, capture_output=True, text=True).stdout


SUITE = "(suite)"  # the whole suite did not finish


def title_for(suite, scenario):
    if scenario == SUITE:
        return "Nightly lab: --suite %s does not finish" % suite
    return "Nightly lab: %s/%s fails" % (suite, scenario)


def reproduce(suite, scenario):
    if scenario == SUITE:
        return "scripts/test/pp_local_test.sh run --suite %s" % suite
    return 'PP_APP_LAB_ARGS="--only %s" scripts/test/pp_local_test.sh run --suite %s' % (scenario, suite)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--summary", action="append", default=[], help="suite=path (path may be missing)")
    ap.add_argument("--sha", required=True)
    ap.add_argument("--run-url", required=True)
    ap.add_argument("--dry-run", action="store_true", help="print what would change; read-only gh calls only")
    args = ap.parse_args()

    failing, passing, flaky, lines = {}, set(), [], []
    for item in args.summary:
        suite, path = item.split("=", 1)
        try:
            with open(path) as f:
                results = json.load(f)["results"]
        except (OSError, ValueError, KeyError):
            failing[(suite, SUITE)] = "the suite did not finish (no summary)"
            lines.append("- **%s**: did not finish" % suite)
            continue
        counts = {"pass": 0, "flaky": 0, "fail": 0}
        for r in results:
            counts[r["result"]] = counts.get(r["result"], 0) + 1
            key = (suite, r["name"])
            if r["result"] == "fail":
                failing[key] = r.get("detail") or "failed"
            else:
                passing.add(key)
                if r["result"] == "flaky":
                    flaky.append("%s/%s (%s)" % (suite, r["name"], r.get("detail") or "failed once"))
        passing.add((suite, SUITE))
        lines.append("- **%s**: %d passed, %d flaky, %d failed" % (suite, counts["pass"], counts["flaky"], counts["fail"]))

    open_issues = json.loads(gh("issue", "list", "--label", LABEL, "--state", "open", "--limit", "200",
                                "--json", "number,title") or "[]")
    by_title = {i["title"]: i["number"] for i in open_issues}
    if failing:
        gh("label", "create", LABEL, "--force", "--color", "B60205",
           "--description", "A scenario the nightly lab finds failing", dry_run=args.dry_run)

    short = args.sha[:9]
    for (suite, scenario), detail in sorted(failing.items()):
        title = title_for(suite, scenario)
        if title in by_title:
            gh("issue", "comment", str(by_title[title]), "--body",
               "Still failing on `%s`: %s\n\n%s" % (short, detail, args.run_url), dry_run=args.dry_run)
        else:
            what = ("`--suite %s` did not finish" % suite if scenario == SUITE else
                    "The nightly lab finds `%s` failing in `--suite %s` (it failed again on its retry)" % (scenario, suite))
            body = ("%s.\n\n"
                    "- First seen on: `%s`\n- Error: %s\n- Run (logs in its artifacts): %s\n\n"
                    "Reproduce: `%s`\n\nThis issue closes itself the night it passes again."
                    % (what, short, detail, args.run_url, reproduce(suite, scenario)))
            gh("issue", "create", "--title", title, "--label", LABEL, "--body", body, dry_run=args.dry_run)
    for title, number in sorted(by_title.items()):
        for key in passing:
            if title == title_for(*key):
                gh("issue", "comment", str(number), "--body",
                   "Passes again on `%s`: %s" % (short, args.run_url), dry_run=args.dry_run)
                gh("issue", "close", str(number), "--reason", "completed", dry_run=args.dry_run)

    report = ["## Nightly lab on `%s`" % short, ""] + lines
    if failing:
        report += ["", "**Failing**"] + ["- %s: %s" % (title_for(s, n)[len("Nightly lab: "):], d) for (s, n), d in sorted(failing.items())]
    if flaky:
        report += ["", "**Flaky** (failed, then passed on the retry)"] + ["- %s" % f for f in flaky]
    text = "\n".join(report) + "\n"
    print(text)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write(text)
    sys.exit(1 if failing else 0)


if __name__ == "__main__":
    main()
