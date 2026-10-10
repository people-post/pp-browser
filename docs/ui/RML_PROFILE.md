# RML profile for AI generation

**Tier:** product / UI

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
- Text is bound as `{{expr}}` in the element's content; the engine sets it as text, so names, titles and message text need no escaping. `data-rml` sets inner RML — the value is parsed as markup and a `{{…}}` inside it is evaluated — so it is only for fields that carry markup built by our code, named `*_rml` (`row.content_rml`, `turn.user_content_rml`, `turn.assistant_content_rml`, `working_set_rml`). `scripts/check/check_rml_bindings.sh` (CI lint) and `rml_binding_guard_test` enforce this in the views and in the markup `src/` serializes. Markup built in C++ escapes text with `common/ui/RmlEscape.h` (or `StructuredTextParser::EscapeText`).
- `data-event-click="action_name()"` — chat chips use `send_chat_action('__ENTRY__', n)`; forms use `submit_form('__ENTRY__', form_id)`; calendar uses `calendar_prev`, `calendar_next`, `select_calendar_day`

## Growing textarea (`max-rows`)

`<textarea rows="2" max-rows="6">` grows with its text between `rows` and `max-rows`, then scrolls. The chat composer uses it. The attribute needs the pp-cpp-ui release **after v0.3.1**; older engines ignore it and keep `rows` (no error), so check `cmake/PpCppUi.cmake` when the composer does not grow.

## Selectable text (pp-browser fork)

Add `selectable="text"` on a static content container to enable drag-selection and Ctrl+C copy. Use `focus: none` on bubbles so the chat input keeps focus. Interactive controls (e.g. suggestion buttons, form fields, calendar days) may live inside selectable regions; elements opt out via `QuerySelection` / `BlocksSelectionInteraction`. Selection spans multiple `selectable="text"` containers in one drag.

## Styling

Use classes from `assets/themes/base.rcss` (`.stack`, `.row`, `.card`, `.text`, `.heading-1`, `.heading-2`, `.btn`, `.btn-primary`, `.field`, `.muted`, `.error`).

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
| `long_list` | `items[]`: `title`, optional `id`, `subtitle`, `meta`, `avatar_letter`, `avatar_tone`, `share_text`, `share_url`, `actions[]` (`style` optional: `primary`\|`secondary`\|`link`); optional `title`, `footer_actions[]` (`primary`\|`secondary` only) | scrollable list + avatar row + suggestion buttons. An item action with `style: "link"` is a small text link at the end of the item's last text paragraph (subtitle, else title) instead of a button in the row below; in `footer_actions` it renders as a normal button. `link` is for blocks the app builds itself (the article feed); it is deliberately not listed in the prompt profile below. `share_text` / `share_url` (also app-built only) become `share-text` / `share-url` attributes on the item; the chat host offers Ask AI / Copy / Share for such an item on long press or right click |
| `choice` | `prompt`, `options[]` | prompt + suggestion buttons |
| `poll` | `question`, `options[]` | poll + suggestion buttons |

Widget blocks (`form`, `calendar`) render binding-aware RML inside the assistant bubble; field values and calendar month state update via the chat data model without rewriting the bubble markup.

Unknown block types are skipped; valid blocks still render. When any block is skipped, a muted footer is shown.

See [CHAT_TEMPLATES.md](CHAT_TEMPLATES.md) for usage guidance.

### Rules

- No HTML, markdown, or arbitrary CSS in responses
- Text is plain UTF-8; the parser escapes special characters before rendering
- Prefer the block types above; unknown types are tolerated but not rendered
