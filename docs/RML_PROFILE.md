# RML profile for AI generation

AI-generated UI must follow this profile.

## Allowed RML elements

`rml`, `head`, `title`, `link`, `body`, `div`, `span`, `p`, `h1`, `h2`, `h3`, `button`, `input`, `textarea`, `select`, `option`, `label`, `ul`, `ol`, `li`, `table`, `tr`, `td`, `th`

## Forbidden

- `<script>`, `<iframe>`, inline event handlers (`onclick=`)
- `javascript:` URLs
- Arbitrary custom `data-*` except RmlUi data-binding attributes

## Data binding

- `data-model` on `body`
- `data-value`, `data-checked`, `data-for`, `data-if`, `data-visible`, `data-rml`
- `data-event-click="action_name()"`

## Styling

Use classes from `assets/themes/base.rcss` (`.stack`, `.row`, `.card`, `.muted`, `.error`).

## Output artifacts

1. `rml` — document
2. `rcss` — optional extra rules
3. `bindings.json` — action → MCP tool mapping
