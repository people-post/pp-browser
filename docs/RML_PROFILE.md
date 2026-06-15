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
- `data-event-click="action_name()"` — chat chips use `send_chat_action('__ENTRY__', n)`; forms use `submit_form('__ENTRY__', form_id)`; calendar uses `calendar_prev`, `calendar_next`, `select_calendar_day`

## Selectable text (pp-browser fork)

Add `selectable="text"` on a static content container to enable drag-selection and Ctrl+C copy. Use `focus: none` on bubbles so the chat input keeps focus. Interactive controls (e.g. suggestion buttons, form fields, calendar days) may live inside selectable regions; elements opt out via `QuerySelection` / `BlocksSelectionInteraction`. Selection spans multiple `selectable="text"` containers in one drag.

## Styling

Use classes from `assets/themes/base.rcss` (`.stack`, `.row`, `.card`, `.muted`, `.error`).

See [RCSS_PROFILE.md](RCSS_PROFILE.md) for the exact list of supported CSS/RCSS properties. AI prompts must not use properties outside that list.

## Output artifacts

1. `rml` — document
2. `rcss` — optional extra rules
3. `bindings.json` — action → MCP tool mapping

## Structured text (chat responses)

For conversational replies (not full UI documents), respond with a single fenced `json` block:

```json
{
  "blocks": [
    { "type": "paragraph", "text": "Plain explanation." },
    { "type": "heading", "level": 2, "text": "Section title" },
    { "type": "list", "ordered": false, "items": ["Item A", "Item B"] },
    { "type": "code", "text": "snippet" }
  ]
}
```

### Block types

| type | fields | RML output |
|------|--------|------------|
| `paragraph` | `text` | `<p>` |
| `heading` | `text`, `level` (1–3) | `<h1>`–`<h3>` |
| `list` | `items` (array), `ordered` (bool) | `<ul>`/`<ol>` + `<li>` |
| `code` | `text` | `<div class="code-block">` |
| `button` | `label`, `message`, optional `payload` | `<button class="chat-suggestion">` with `send_chat_action` |
| `card` | `title`, `body`, optional `subtitle`, `variant` | `<div class="chat-card">` |
| `table` | `headers[]`, `rows[][]` | `<table class="chat-table">` |
| `key_value` | `items[]`: `label`, `value` | `<div class="chat-key-value">` |
| `callout` | `text`, optional `variant` | `<div class="chat-callout">` |
| `quote` | `text`, optional `attribution` | `<blockquote class="chat-quote">` |
| `form` | `id`, `fields[]`, `submit_template`, optional `title`, `submit_label` | reactive `data-value` form widget |
| `calendar` | optional `month`, `year` (default: today); optional `min_date`, `max_date`, `available_days[]` | reactive calendar table with month nav |
| `action_list` | `items[]` with nested `actions[]` | list + suggestion buttons |
| `choice` | `prompt`, `options[]` | prompt + suggestion buttons |
| `poll` | `question`, `options[]` | poll + suggestion buttons |

Widget blocks (`form`, `calendar`) render binding-aware RML inside the assistant bubble; field values and calendar month state update via the chat data model without rewriting the bubble markup.

Unknown block types are skipped; valid blocks still render. When any block is skipped, a muted footer is shown.

See [CHAT_TEMPLATES.md](CHAT_TEMPLATES.md) for usage guidance.

### Rules

- No HTML, markdown, or arbitrary CSS in responses
- Text is plain UTF-8; the parser escapes special characters before rendering
- Prefer the block types above; unknown types are tolerated but not rendered
