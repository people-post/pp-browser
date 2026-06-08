#include "agent/PromptBuilder.h"

#include <sstream>

namespace pbr {

std::string PromptBuilder::DefaultRcssProfile() {
  return R"(Supported RCSS properties only (RmlUi):
- Box: margin, padding, width, height, min/max-width, min/max-height, box-sizing
- Border: border, border-width, border-color, border-radius
- Layout: display (block, inline, inline-block, flex, inline-flex, flow-root, table*), position, top/right/bottom/left, float, clear, overflow, visibility, z-index
- Flex: flex, flex-grow, flex-shrink, flex-basis, flex-direction, flex-wrap, justify-content, align-items, align-content, align-self, gap
- Text: font-*, color, line-height, text-align, text-decoration, text-transform, text-overflow, white-space, word-break, vertical-align
- Visual: background-color, opacity, box-shadow, transform, filter
- Units: dp, px, %, em

Not supported: resize, CSS grid, background-image/gradients, border-style, @media, pseudo-elements, fit-content, vw/vh/rem.
Prefer existing classes from base.rcss (.stack, .row, .card, .muted, .error) before adding rules.
Avoid inline-block containers with background; use display:block or flex instead.)";
}

std::string PromptBuilder::ChatBlocksProfile() {
  return R"(ALLOWED OUTPUT
Respond with exactly one fenced ```json block:
{
  "blocks": [ ... ]
}

Each entry in blocks MUST be an object with a "type" field.
Use ONLY the four block types below. Any other type will not render.

1. paragraph
   Fields: type (required), text (required, string)
   Renders: plain paragraph text
   Example: { "type": "paragraph", "text": "Plain explanation." }

2. heading
   Fields: type (required), level (required, integer 1-3), text (required, string)
   Renders: h1 (level 1), h2 (level 2), or h3 (level 3)
   Example: { "type": "heading", "level": 2, "text": "Section title" }

3. list
   Fields: type (required), items (required, array of strings), ordered (optional, boolean, default false)
   Renders: bullet list (ordered=false) or numbered list (ordered=true)
   Example: { "type": "list", "ordered": false, "items": ["Item A", "Item B"] }

4. code
   Fields: type (required), text (required, string)
   Renders: monospace code block
   Example: { "type": "code", "text": "int x = 1;" }

NOT SUPPORTED
- Block types other than paragraph, heading, list, code
- HTML tags, markdown, bold, italic, links, tables, images, blockquotes
- Formatting inside paragraph text (no **bold**, no `backticks`, no # headings, no bullet characters)
- Bare strings or other shapes in the blocks array
- Describing structure in prose instead of emitting blocks (e.g. do not write "use H2" — emit a heading block)

RULES
- One visual element per block: titles → heading, bullets → list, code → code, body copy → paragraph
- text and items values are plain UTF-8 only
- Do not wrap the response in HTML or markdown outside the json fence)";
}

std::string PromptBuilder::BuildUiGenerationPrompt(const std::string& tools_context,
                                                   const std::string& rml_profile) {
  std::ostringstream out;
  out << "You generate RML, RCSS, and a bindings JSON manifest for pp-browser.\n\n";
  out << "RML profile rules:\n" << rml_profile << "\n\n";
  out << "RCSS profile rules:\n" << DefaultRcssProfile() << "\n\n";
  out << "Available MCP tools:\n" << tools_context << "\n\n";
  out << "Respond with three fenced blocks in order: ```rml, ```rcss, ```json (bindings manifest).\n";
  out << "Use data-model, data-value, data-event-click, and data-for. No script tags or inline JS.\n";
  return out.str();
}

std::string PromptBuilder::BuildChatSystemPrompt() {
  std::ostringstream out;
  out << "You are a helpful assistant in pp-browser, a native UI shell.\n";
  out << "Replies render as structured blocks — not HTML, not markdown.\n\n";
  out << ChatBlocksProfile() << "\n\n";
  out << "Example response:\n```json\n";
  out << R"({
  "blocks": [
    { "type": "heading", "level": 2, "text": "Overview" },
    { "type": "paragraph", "text": "Here are the supported block types." },
    { "type": "list", "ordered": false, "items": ["paragraph", "heading", "list", "code"] },
    { "type": "code", "text": "function hello() {\n  return 42;\n}" }
  ]
})";
  out << "\n```\n";
  return out.str();
}

} // namespace pbr
