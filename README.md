# PP

**PP** is an AI-centric browsing shell (repo codename: `pp-browser`) built with **SDL3** and a **hard-forked [RmlUi](https://github.com/mikke89/RmlUi)** (RML/RCSS UI).

Product tagline: *The internet, rendered for you.*

Branding and the PP vs pp-browser naming rule: [docs/ui/PRODUCT_BRANDING.md](docs/ui/PRODUCT_BRANDING.md).

## Features

- Cross-platform native window (Linux, Windows, macOS)
- Vendored RmlUi source under `src/lib/rmlui/`
- MCP-oriented action routing and LLM UI generation scaffolding

## Build

See [docs/ops/BUILD.md](docs/ops/BUILD.md). Documentation map: [docs/README.md](docs/README.md).

## Third-party notice

RmlUi is vendored at `src/lib/rmlui/` (MIT). Provenance: `src/lib/rmlui/UPSTREAM.json`.

Lato font in `assets/fonts/` is from RmlUi samples (SIL Open Font License).
