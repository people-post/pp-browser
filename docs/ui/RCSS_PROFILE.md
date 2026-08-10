# RCSS profile for AI generation

**Tier:** product / UI

pp-browser uses [RmlUi RCSS](https://mikke89.github.io/RmlUiDoc/pages/rcss.html). Only properties listed below are supported.

## Units

Use `dp`, `px`, `%`, or `em`. Prefer `dp` for layout (matches the theme sheets).

## Supported properties

### Box model

- `margin`, `margin-top`, `margin-right`, `margin-bottom`, `margin-left` (`auto`, lengths, `%`)
- `padding`, `padding-top`, `padding-right`, `padding-bottom`, `padding-left`
- `width`, `min-width`, `max-width` (`auto`, `none`, lengths, `%`)
- `height`, `min-height`, `max-height` (`auto`, `none`, lengths, `%`)
- `box-sizing` (`content-box`, `border-box`)

### Border

- `border`, `border-top`, `border-right`, `border-bottom`, `border-left` (width + color only)
- `border-width`, `border-color`
- `border-radius`, corner radius properties

### Layout

- `display`: `none`, `block`, `inline`, `inline-block`, `flow-root`, `flex`, `inline-flex`, table values
- `position`: `static`, `relative`, `absolute`, `fixed`, `sticky`
- `top`, `right`, `bottom`, `left`, `inset`
- `float`: `none`, `left`, `right`
- `clear`: `none`, `left`, `right`, `both`
- `overflow`, `overflow-x`, `overflow-y`: `visible`, `hidden`, `auto`, `scroll`
- `visibility`: `visible`, `hidden`
- `z-index`

### Flexbox

- `flex`, `flex-grow`, `flex-shrink`, `flex-basis`, `flex-direction`, `flex-wrap`, `flex-flow`
- `justify-content`, `align-items`, `align-content`, `align-self`
- `gap`, `row-gap`, `column-gap`

### Typography

- `font`, `font-family`, `font-style`, `font-weight`, `font-size`, `font-kerning`
- `color`, `line-height`, `letter-spacing`
- `text-align`: `left`, `right`, `center`, `justify`
- `text-decoration`, `text-transform`, `text-overflow`
- `white-space`: `normal`, `pre`, `nowrap`, `pre-wrap`, `pre-line`
- `word-break`: `normal`, `break-all`, `break-word`
- `vertical-align`

### Background and opacity

- `background`, `background-color`
- `opacity`

### Text selection styling

Browsers use `::selection`; RmlUi does not. Instead, style the hidden `selection` child via descendant selectors (see `assets/themes/colors-light.rcss` / `colors-dark.rcss`):

- `selection { background-color: ...; color: ...; }` — default highlight
- `.bubble-user selection { ... }` — per-context overrides on `selectable="text"` containers and `input`/`textarea` parents

Applies to static bubble text and focused form fields.

### Other

- `cursor`, `clip`, `box-shadow`
- `transform`, `transform-origin`, `perspective`, `perspective-origin`
- `transition`, `animation`
- `filter`, `backdrop-filter`
- `pointer-events`, `caret-color`, `image-color`

## Not supported (do not use)

- `resize`
- CSS Grid (`grid-*`)
- `background-image`, gradients, `url(...)`
- `border-style` (RmlUi borders are width + color only)
- `@media` (app theme sheets only — not for AI-generated RCSS), `@keyframes`, pseudo-elements (`::before`), complex selectors
- Vendor prefixes (`-webkit-`, etc.)
- `fit-content`, `max-content`, `min-content`
- `border-collapse`, `border-spacing`, `table-layout`
- `vw`, `vh`, `rem` (use `dp`, `px`, `%`, `em`)

## Layout guidance for AI-generated UI

- Prefer block layout and flexbox; avoid `inline-block` for sized containers with backgrounds
- Put text and background on the same element, or use `display: block` wrappers
- Use existing utility classes from the design system before adding new rules
- Do not emit raw `#hex` colors in AI-generated RCSS; see [UI_DESIGN_SYSTEM.md](UI_DESIGN_SYSTEM.md)
- Do not rely on `text-align` for block sizing; use flex `justify-content` or `float`

## Design system classes

See [UI_DESIGN_SYSTEM.md](UI_DESIGN_SYSTEM.md) for the full token table and surface inventory.

### Layout and typography

`.stack`, `.row`, `.gap-sm`, `.text`, `.text-xs`, `.heading-1`, `.heading-2`, `.heading-3`, `.muted`, `.error`

### Controls

`.btn`, `.btn-primary`, `.btn-secondary`, `.field`, `.card`

### Chat and shell

`.bubble-user`, `.bubble-assistant`, `.bubble-peer`, `.prompt-composer`, `.chat-suggestion`, `.chat-form`, `.chat-callout`, `.chat-callout-warning`, `.chat-card`, `.chat-card-highlight`, `.chat-working-set-chip`, `.shell-toolbar-btn`, `.sidebar-session`

Theme colors are applied via `@media (theme: light|dark)` in app stylesheets only — not in AI output.

**Compact floating chrome** utilities (`.surface-chrome`, `.surface-chrome--frost`, `.surface-chrome--solid`) are **theme-only** — defined in `assets/themes/components.rcss` and `colors-*.rcss`. Do not emit `backdrop-filter` or these classes in AI-generated RCSS/RML.

Author stylesheets should use these classes instead of bare `h1`, `p`, `button`, `input`, etc. The user-agent stylesheet provides baseline layout for semantic elements; only `.bubble-assistant` scopes rich-text overrides for AI-generated markup.
