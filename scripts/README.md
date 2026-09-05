# Scripts layout

Repo tooling lives under topic subdirs (not a flat dump). Prefer these paths in docs, CI, and agent commands.

| Dir | Purpose | Entry points |
|-----|---------|--------------|
| [`check/`](check/) | Layer / include / platform `#ifdef` guards (CI lint) | `check_base_includes.sh`, `check_feature_includes.sh`, `check_platform_ifdefs.sh`, … |
| [`platform/`](platform/) | Mobile / desktop build, sign, notarize; Linux `pp-node` package | `android_build.sh`, `ios_build.sh`, `macos_sign_and_notarize.sh`, `pp_node_package_linux.sh` |
| [`vendor/`](vendor/) | Import / refresh third-party and fork trees | `vendor_import.sh`, `libp2p_vendor_import.sh`, `fonts_import_noto.sh`, `rmlui_tests_import.sh` |
| [`test/`](test/) | Local driver + image / hop / hard-lab / call smokes | `pp_local_test.sh`, `pp_*_smoke.sh`, `pp_hard_*`, `*_lib.sh` |
| [`dev/`](dev/) | Local profile wipe after hard cuts | `wipe_local_profile.sh`, `wipe_local_profile.ps1` |

Examples:

```bash
./scripts/check/check_feature_includes.sh
./scripts/vendor/vendor_import.sh
./scripts/platform/pp_node_package_linux.sh all
./scripts/test/pp_local_test.sh run --suite hard
./scripts/dev/wipe_local_profile.sh --dry-run
```

Doctrine / purpose IDs: [docs/architecture/TESTING.md](../docs/architecture/TESTING.md), [docs/ops/TEST_STRATEGY.md](../docs/ops/TEST_STRATEGY.md). Hard lab: [packaging/pp-node/HARD_LAB.md](../packaging/pp-node/HARD_LAB.md).
