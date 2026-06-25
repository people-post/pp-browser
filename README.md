# pp-browser

AI-native browser shell built with **SDL3** and a **hard-forked [RmlUi](https://github.com/mikke89/RmlUi)** (RML/RCSS UI).

## Features

- Cross-platform native window (Linux, Windows, macOS)
- Vendored RmlUi source under `src/render/fork/`
- MCP-oriented action routing and LLM UI generation scaffolding
- Interactive search demo: `./build/pp-browser --demo search`

## Build

See [docs/BUILD.md](docs/BUILD.md).

## Third-party notice

RmlUi is vendored at `src/render/fork/` (MIT). Provenance: `src/render/fork/UPSTREAM.json`.

Lato font in `assets/fonts/` is from RmlUi samples (SIL Open Font License).
