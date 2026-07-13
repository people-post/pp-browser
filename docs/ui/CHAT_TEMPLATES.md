# Chat reply templates

**Tier:** product / UI

Assistant replies use structured JSON blocks parsed by [`StructuredTextParser`](../../src/base/ai/StructuredTextParser.cpp) into inline RML inside selectable bubbles.

## Block categories

| Category | Blocks | Binding |
|----------|--------|---------|
| Static | paragraph, heading, list, code, card, table, key_value, callout, quote | Escaped HTML only |
| Click actions | button, action_list, choice, poll, long_list | `send_chat_action(entry, index)` |
| Reactive widgets | form, calendar | `data-value`, `data-for`, `data-if` on chat or working set model |

## Working set panel

Panel-eligible blocks (`long_list`, `form`, `calendar`, large `table`, etc.) render as a **compact chip in chat** and open in the **auxiliary working set panel** (see [WORKING_SET_PANEL.md](WORKING_SET_PANEL.md)).

- Chat keeps narrative blocks (paragraph, heading) and teaser chips (`open_working_set(entry, block_index)`).
- The panel shows the full artifact (uncapped list, expanded form, calendar grid).
- Forms and calendars in the panel bind to `working_set.*` on the shell data model.

## Long list (LLM + MCP)

`long_list` is a **presentation** block for scrollable feeds. The LLM discovers sources via MCP tool schemas, calls tools through function calling, maps the response into normalized `items[]`, then emits the block in its final reply.

- **Row fields:** `title` (required), optional `id`, `subtitle`, `meta`
- **Row actions:** `items[].actions[]` with `label`, `message`, optional `payload`
- **Pagination:** `footer_actions[]` (e.g. "More" with `payload` containing `before_id`) → user message → LLM fetches next page via MCP → new `long_list` reply

Reference example: `blog_articles` MCP tool → map `{ articles: [...] }` into rows.

## Reactive widgets

Form and calendar blocks emit **stable RML templates** with data bindings. In the messages layout, chat teasers point to the working set panel where forms bind under `working_set.*` on the shell model. C++ widget state lives in `ChatController::widgets_by_entry_`.

### Form

- Fields bind via `data-value="field.value"` (checkbox uses `data-checked="field.checked"`)
- Submit calls `submit_form(entry_id, form_id)`; values read from bound state (not DOM scrape)
- Only the latest unsubmitted form stays editable; older forms set `form.expired = true`

### Calendar

- Month label and day grid bind to `turn.calendar.*`
- Prev/next call `calendar_prev` / `calendar_next`
- Day click calls `select_calendar_day(entry_id, iso_date)` and sends `Selected YYYY-MM-DD`

## Mock chat triggers

Type these in mock mode (no LLM): `help`, `list`, `code`, `button`, `form`, `calendar`, `card`, `poll`

## LLM prompt

Full block catalog is in [`PromptBuilder::ChatBlocksProfile()`](../../src/base/ai/PromptBuilder.cpp). See also [`RML_PROFILE.md`](RML_PROFILE.md).
