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

- CI **compiles** iOS (the `ios` job) but runs nothing there. Code that builds can still do nothing on a
  device: SDL3's iOS file dialog is a stub that answers "nothing picked", RmlUi strips the leading `/` of an
  absolute path (the iOS working directory is the app bundle), and a `font-style` / `font-weight` with no
  loaded face draws no text. When a change touches **file pickers, images shown from the data dir, or
  font styles**, ask for an iPhone check in **Testing** (2026-10-20: all three were found on a device only).
- Changing what a **shared function** does (e.g. `PrepareThreadHistoryClear`, used by clear history *and*
  clear downloaded media): the author lists every caller and says why the new behaviour is right for each.
- A pull request that is **CONFLICTING** against `develop` never triggers the `pull_request` checks; "no checks
  reported" usually means "rebase first", not "CI is down".
