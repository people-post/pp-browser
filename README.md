# Frame

**Frame** is an AI-centric browsing shell (repo codename: `pp-browser`) built with **SDL3** and a **hard-forked [RmlUi](https://github.com/mikke89/RmlUi)** (RML/RCSS UI).

Product tagline: *The internet, rendered for you.*

Branding rationale, icon mockups, and review checklist: [docs/ui/PRODUCT_BRANDING.md](docs/ui/PRODUCT_BRANDING.md).

## Features

- Cross-platform native window (Linux, Windows, macOS)
- Vendored RmlUi source under `src/render/fork/`
- MCP-oriented action routing and LLM UI generation scaffolding

## Build

See [docs/ops/BUILD.md](docs/ops/BUILD.md). Documentation map: [docs/README.md](docs/README.md).

## Third-party notice

RmlUi is vendored at `src/render/fork/` (MIT). Provenance: `src/render/fork/UPSTREAM.json`.

Lato font in `assets/fonts/` is from RmlUi samples (SIL Open Font License).
