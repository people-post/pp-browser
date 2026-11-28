#!/usr/bin/env python3
"""Tests of the app lab's scenario tags (app_lab_select.py), of its registry (pp_app_lab.py's SCENARIOS) and of the
config.json it writes for each user.

  python3 -m unittest discover -s scripts/test -p '*_test.py'

Standard library only (PyYAML, when present, adds the check that BELOW matches build.yml's capability filters).
Needs no lab: nothing here starts a probe or Docker.
"""

import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import app_lab_select as S  # noqa: E402
import pp_app_lab as lab  # noqa: E402

EVERYTHING = set(S.VOCABULARY)


# pp-cpp-core's capabilities (projects/capability-repos CR027): tagged here (the lab's scenarios name them), their
# folders in pp-cpp-core.
CORE = {"base", "product", "people", "net", "node"}


class CapabilityOrderTest(unittest.TestCase):
    def test_above_follows_the_capability_order(self):
        self.assertEqual(S.above("media"), {"media", "messaging", "calls", "broadcast"})
        self.assertEqual(S.above("calls"), {"calls", "broadcast"})
        self.assertEqual(S.above("messaging"), {"messaging"})
        self.assertEqual(S.above("people"), set(S.CAPABILITY_TAGS) - {"media", "product", "ledger"})
        self.assertEqual(S.above("net"), {"net", "delivery", "pricing", "messaging", "calls", "broadcast", "node", "relay"})

    def test_every_capability_folder_has_a_tag(self):
        folders = {d for d in os.listdir(os.path.join(ROOT, "capabilities"))
                   if os.path.isdir(os.path.join(ROOT, "capabilities", d))}
        self.assertEqual(folders | CORE - {"base"}, set(S.CAPABILITY_TAGS))
        self.assertEqual(set(S.BELOW), set(S.CAPABILITY_TAGS))

    def test_below_matches_the_ci_capability_filters(self):
        try:
            import yaml
        except ImportError:
            self.skipTest("PyYAML not installed")
        with open(os.path.join(ROOT, ".github", "workflows", "build.yml")) as f:
            workflow = yaml.safe_load(f)
        step = next(s for s in workflow["jobs"]["changes"]["steps"] if s.get("id") == "filter")
        filters = yaml.safe_load(step["with"]["filters"])

        def flat(items):
            for item in items:
                if isinstance(item, list):
                    yield from flat(item)
                else:
                    yield item

        # pp-cpp-core's folders are not in this tree: a change to them arrives by the pin (cmake/), the base's filter.
        for capability in S.CAPABILITY_TAGS:
            folders = {p.split("/")[1] for p in flat(filters[capability]) if p.startswith("capabilities/")}
            self.assertEqual(folders, (S.below(capability) | {capability}) - CORE, capability)


class PathTableTest(unittest.TestCase):
    def tags(self, path):
        return S.tags_for_path(path)[0]

    def test_capability_folders(self):
        self.assertEqual(self.tags("capabilities/calls/src/lib/calls/CallSession.cpp"), {"calls", "broadcast"})
        self.assertEqual(self.tags("capabilities/node/CMakeLists.txt"), {"node"})
        self.assertEqual(self.tags("capabilities/people/src/lib/people/ContactsStore.h"), S.above("people"))
        self.assertEqual(self.tags("capabilities/base/src/foundation/crypto/Seal.cpp"), EVERYTHING)
        self.assertEqual(self.tags("capabilities/newthing/src/x.cpp"), EVERYTHING)

    def test_build_and_pins_pick_everything(self):
        for path in ("cmake/PpCppMesh.cmake", "CMakeLists.txt", "src/CMakeLists.txt", "third_party/curl/x.c"):
            self.assertEqual(self.tags(path), EVERYTHING, path)

    def test_docs_tests_and_gui_pick_nothing(self):
        for path in ("docs/ops/TEST_STRATEGY.md", "projects/release-compatibility/DESIGN.md", "README.md",
                     "capabilities/calls/README.md", "src/gui/CallController.cpp", "assets/views/chat.rml",
                     "src/lib/ai/PromptBuilder.cpp", "capabilities/calls/src/lib/calls/tests/CallTest.cpp",
                     ".github/workflows/build.yml", "scripts/check/check_format.sh", "android/app/build.gradle",
                     "src/app/Application.cpp", "scripts/test/pp_hard_nat_smoke.sh"):
            self.assertEqual(self.tags(path), set(), path)

    def test_the_app_stack_and_the_lab_pick_everything(self):
        for path in ("src/feature/product/ProductMesh.cpp", "src/feature/presenters/CallPresenter.cpp",
                     "src/foundation/data/Paths.cpp", "src/lib/storage/SqliteThreadStore.cpp", "src/app/Bootstrap.cpp",
                     "src/app/probes/app_probe/main.cpp", "scripts/test/pp_app_lab.py",
                     "packaging/pp-node/docker-compose.app-lab.yml", "packaging/pp-node/app-lab-tls/proxy.js",
                     "somewhere/new.txt"):
            self.assertEqual(self.tags(path), EVERYTHING, path)

    def test_seeds_gateways_and_workflows(self):
        self.assertEqual(self.tags("apps/node/src/app/node/NodeBootstrap.cpp"), {"node"})
        self.assertEqual(self.tags("packaging/pp-node/Dockerfile"), {"node"})
        self.assertEqual(self.tags("packaging/pp-node/Dockerfile.hard-gw"), {"mesh"})
        self.assertEqual(self.tags(".github/workflows/lab-smoke.yml"), {"smoke"})
        self.assertEqual(self.tags("./docs/x.md"), set())

    def test_paths_add_up_and_say_why(self):
        tags, lines = S.tags_for_paths(["docs/a.md", "docs/b.md", "capabilities/calls/src/x.cpp"])
        self.assertEqual(tags, {"calls", "broadcast"})
        self.assertEqual(len(lines), 2)
        self.assertIn("2 paths (docs/a.md, docs/b.md): docs → none", lines)
        self.assertEqual(S.tags_for_paths([]), (set(), []))


class ChangedPathsTest(unittest.TestCase):
    def test_a_list_of_paths(self):
        self.assertEqual(S.changed_paths("docs/a.md,src/gui/b.cpp docs/a.md", ROOT), ["docs/a.md", "src/gui/b.cpp"])
        self.assertEqual(S.changed_paths("-", ROOT, stdin=["x/y\n", "\n", "z\n"]), ["x/y", "z"])

    def test_git_ranges_and_a_branch(self):
        with tempfile.TemporaryDirectory() as repo:
            def git(*args):
                subprocess.run(["git", "-C", repo, "-c", "user.name=t", "-c", "user.email=t@t", *args], check=True,
                               capture_output=True)

            def write(path):
                os.makedirs(os.path.dirname(os.path.join(repo, path)) or repo, exist_ok=True)
                with open(os.path.join(repo, path), "a") as f:
                    f.write("x\n")

            git("init", "-q", "-b", "main")
            write("docs/a.md")
            git("add", "-A")
            git("commit", "-qm", "one")
            git("checkout", "-qb", "work")
            write("capabilities/calls/x.cpp")
            git("add", "-A")
            git("commit", "-qm", "two")
            write("docs/a.md")  # uncommitted
            write("src/gui/new.cpp")  # untracked
            self.assertEqual(S.changed_paths("main...work", repo), ["capabilities/calls/x.cpp"])
            self.assertEqual(sorted(S.changed_paths("main", repo)),
                             ["capabilities/calls/x.cpp", "docs/a.md", "src/gui/new.cpp"])
            with self.assertRaises(SystemExit):
                S.changed_paths("nosuch...work", repo)


class SelectTest(unittest.TestCase):
    ENTRIES = [("a", ["calls", "smoke"]), ("b", ["messaging"]), ("c", ["calls", "mesh"]), ("d", ["node"])]
    SKIPS = {"c": "needs --nat"}

    def picked(self, **kw):
        return [name for name, _, selected, _ in S.select(self.ENTRIES, self.SKIPS, **kw) if selected]

    def test_default_run_is_every_scenario_the_mode_runs(self):
        self.assertEqual(self.picked(), ["a", "b", "d"])

    def test_tags_pick_any_of_them_within_the_mode(self):
        self.assertEqual(self.picked(tags={"calls"}), ["a"])
        self.assertEqual(self.picked(tags={"messaging", "node"}), ["b", "d"])
        rows = {r[0]: r for r in S.select(self.ENTRIES, self.SKIPS, tags={"calls"})}
        self.assertEqual(rows["c"][3], "needs --nat (picked by --tags calls)")
        self.assertEqual(rows["b"][3], "not picked")

    def test_only_tags_and_changed_add_up(self):
        self.assertEqual(self.picked(only=["c"], tags={"messaging"}, changed={"node"}), ["b", "c", "d"])
        rows = {r[0]: r for r in S.select(self.ENTRIES, self.SKIPS, only=["c"], changed={"node"})}
        self.assertEqual(rows["c"][3], "--only")
        self.assertEqual(rows["d"][3], "--changed node")

    def test_a_change_with_no_tags_picks_nothing(self):
        self.assertEqual(self.picked(changed=set()), [])
        self.assertEqual(self.picked(changed=EVERYTHING), ["a", "b", "d"])

    def test_unknown_names_and_tags_are_errors(self):
        with self.assertRaises(SystemExit) as e:
            S.select(self.ENTRIES, self.SKIPS, only=["aa"])
        self.assertIn("did you mean a?", str(e.exception))
        with self.assertRaises(SystemExit):
            S.parse_tags(["calls,nosuch"])
        self.assertEqual(S.parse_tags(["calls,groups", "smoke"]), {"calls", "groups", "smoke"})

    def test_the_listing_counts_what_was_not_picked(self):
        lines = S.format_selection(S.select(self.ENTRIES, self.SKIPS, tags={"calls"}), everything=True)
        self.assertEqual(lines[-1], "1 of 4 scenarios selected (2 not picked)")
        self.assertEqual(len(lines), 3)  # a (run), c (left out by the mode), the count


class RegistryTest(unittest.TestCase):
    def test_the_lab_registry_passes_its_self_check(self):
        self.assertEqual(S.check_registry(lab.scenario_entries()), [])

    def test_the_self_check_finds_missing_and_unknown_tags(self):
        problems = S.check_registry([("a", ["smoke"]), ("b", ["calls", "nosuch"]), ("b", ["calls"])])
        self.assertTrue(any(p.startswith("a: no capability tag") for p in problems))
        self.assertTrue(any(p.startswith("b: unknown tag nosuch") for p in problems))
        self.assertIn("b: declared twice", problems)
        self.assertIn("no scenario is tagged smoke", S.check_registry([("a", ["calls"])]))

    def test_every_scenario_is_tagged(self):
        names = [name for name, _, _ in lab.SCENARIOS]
        self.assertEqual(len(names), len(set(names)))
        self.assertEqual(set(lab.SCENARIO_FNS), set(names))
        for name, _, tags in lab.SCENARIOS:
            self.assertTrue(tags.split(), name)

    def test_the_smoke_set_runs_whole_in_the_app_suite(self):
        smoke = [name for name, tags in lab.scenario_entries() if "smoke" in tags]
        rows = S.select(lab.scenario_entries(), lab.default_skips(nat=False), tags={"smoke"})
        self.assertEqual([r[0] for r in rows if r[2]], smoke)
        self.assertLessEqual(len(smoke), 12)  # lab smoke's budget is 15 min (DESIGN § Regression suites)

    def test_the_default_runs_are_unchanged_by_tags(self):
        for nat in (False, True):
            skipped = lab.OPT_IN | (lab.NEEDS_DIRECT_PATH | lab.NEEDS_COLD_SEED_LINK | lab.NEEDS_TLS_RELAYS if nat
                                    else lab.NEEDS_NAT)
            want = [name for name, _, _ in lab.SCENARIOS if name not in skipped]
            rows = S.select(lab.scenario_entries(), lab.default_skips(nat))
            self.assertEqual([r[0] for r in rows if r[2]], want, "nat=%s" % nat)


class LabConfigTest(unittest.TestCase):
    """What a lab user's config.json names: its custom backend's table (profile-model PM002, docs/ops/CONFIGURATION.md
    `backend.services`). A service the table does not name is absent in the app, so a config without the relay services
    leaves every restore, linked device and locker scenario with "No locker provider configured"."""

    class FakeLab:
        user_relay = "http://127.0.0.1:18719/api/relay"
        hop = "/ip4/198.51.100.10/udp/443/adp/2/p2p/QmHop"
        tls_relays = {"a": {"origin": "https://relay-a.lab.test/api/relay"},
                      "b": {"origin": "https://relay-b.lab.test/api/relay"}}

        def relay_id(self, relay):
            return "relay:" + relay

        def relay_link(self, relay):
            return lab.Lab.relay_link(self, relay)

    def test_a_user_names_the_lab_relays_services(self):
        fake = self.FakeLab()
        config = lab.Lab.config_for(fake, "/tmp/user")
        self.assertEqual(config["backend"]["env"], "custom")
        self.assertEqual(config["backend"]["services"], {
            "account_locker": config["registration"]["base_url"],
            "client_compat": config["relay"]["base_url"],
            "crash_intake": config["relay"]["base_url"]})
        self.assertEqual(config["relay"]["base_url"], fake.user_relay)
        for llm in ("llm_gateway", "llm_token"):
            self.assertNotIn(llm, config["backend"]["services"], "the lab has no LLM service")

    def test_a_user_on_the_tls_relays_names_its_preferred_relays_services(self):
        fake = self.FakeLab()
        config = lab.Lab.config_for(fake, "/tmp/user")
        lab.Lab.on_tls_relays(fake, config, ["b", "a"], ["b"])
        preferred = fake.tls_relays["b"]["origin"]
        self.assertEqual(config["registration"]["base_url"], preferred)
        self.assertEqual(config["backend"]["services"], {
            "account_locker": preferred, "client_compat": preferred, "crash_intake": preferred})


if __name__ == "__main__":
    unittest.main()
