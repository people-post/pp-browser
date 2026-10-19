# Review guide for pp-browser

Read by code reviewers, human or automated, before commenting on a pull request. Keep it short; project
facts live in [AGENTS.md](AGENTS.md) and [docs/](docs/README.md).

## What failure looks like here

Flag these first; everything else is secondary.

| Area | A real failure is… | Evidence to cite |
|---|---|---|
| `docs/contracts/` | a wire / disk / HTTP format changed in `src/` without the contract updated in the same PR, or a contract that no longer matches the code ("it lies") | the struct / codec / route and the contract line |
| Platform code (`*_Darwin`, `*_Ios`, `*_Android`, `*_Win32`, `*_Linux`, `*_Posix`, `*_Default`) | a new platform file missing from one of the CMake platform branches; an OS `#ifdef` in a file without a platform suffix; Objective-C++ that assumes ARC (the platform target compiles without it) | the CMake branch and the file |
| Threading / ownership ([THREADING.md](docs/architecture/THREADING.md), [OWNERSHIP.md](docs/architecture/OWNERSHIP.md)) | RmlUi / shell state touched off the UI thread; a blocking call on the UI thread; an owner used after its thread stopped | the call site and the rule |
| First-run / unlock / settings UI | a new view not registered in `ViewCatalog`; a new `ShellTypes` field without its `Bind` + `Dirty` in `ShellHost`; a data model key missing from the controller's `DirtyAll`; a flow that only works on a profile that already exists | the registration table / `DirtyAll` and the view |
| User-facing text (`assets/locales/*.json`) | protocol or implementation terms shown to users (relay, peer id, payload, fingerprint, PSK, epoch, mesh); a key added to one language only | the key in both files |
| Metrics / logs | an event line carrying an account id, peer id, nickname, address or message content | the `MetricsLine` / log statement |
| Security surface (`foundation/crypto`, `foundation/identity`, `domain/people/IdentityStore*`, `feature/registration`, local backup) | secrets written in the clear outside the vault / Keychain; a signed request whose sign bytes differ between client and gateway; a path that skips the PIN | the bytes builder on both sides |
| Tests | a behaviour change with no test, or a test that asserts the old wording / order the PR just changed | the test file |

## How to write findings

- Cite a file and line for every finding. No findings from names, PR text or earlier turns alone.
- Start the summary with a count ("3 findings, 1 important"). When there is nothing to fix, say so in one line.
- Tag each finding **Important**, **Nit** or **Pre-existing**. At most 3 nits per review; drop style-only and
  diagnostic-only comments.
- Do not repeat a finding already posted on the PR. After three review rounds, ask the author to reconsider
  the approach instead of posting another round.

## Skip

- `third_party/`, `build*/`, `install-*/`, generated files, `assets/fonts/`, `assets/icons/`.
- Dependabot and Amp / ui pin bumps unless a `docs/contracts/` file or a platform branch changed with them.

## Known gaps a reviewer should remember

- CI builds macOS, Linux, Windows and Android, **not iOS**. iOS-only code (`*_Ios.*`, Objective-C++) is only
  verified by a local device build; ask for the build log line in **Testing**.
- A pull request that is **CONFLICTING** against `develop` never triggers the `pull_request` checks; "no checks
  reported" usually means "rebase first", not "CI is down".
