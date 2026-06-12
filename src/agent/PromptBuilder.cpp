#include "agent/PromptBuilder.h"

#include <nlohmann/json.hpp>

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
Use ONLY the five block types below. Any other type will not render.

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

5. button
   Fields: type (required), label (required, string), message (required, string)
   Renders: clickable suggestion chip; clicking sends message as the user's next message
   Example: { "type": "button", "label": "Explain more", "message": "Can you explain that in simpler terms?" }
   Use sparingly for 1-3 suggested follow-ups. message is plain text (no markdown).

NOT SUPPORTED
- Block types other than paragraph, heading, list, code, button
- HTML tags, markdown, bold, italic, links, tables, images, blockquotes
- Formatting inside paragraph text (no **bold**, no `backticks`, no # headings, no bullet characters)
- Bare strings or other shapes in the blocks array
- Describing structure in prose instead of emitting blocks (e.g. do not write "use H2" — emit a heading block)
- Inventing action names or HTML in button blocks

RULES
- One visual element per block: titles → heading, bullets → list, code → code, body copy → paragraph, follow-ups → button
- text and items values are plain UTF-8 only
- Emit valid JSON only: every { and [ must be closed; the root object must end with ]}
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

std::string PromptBuilder::BuildChatAgentSystemPrompt(const std::string& tools_summary) {
  std::ostringstream out;
  out << "You are a helpful assistant in pp-browser, a native UI shell.\n";
  out << "Replies render as structured blocks — not HTML, not markdown.\n\n";

  if (!tools_summary.empty()) {
    out << "AVAILABLE TOOLS\n" << tools_summary << "\n";
    out << "TOOL USE RULES\n";
    out << "- You MUST call web_search before answering when the user asks about today, latest, current, live, "
           "or real-time information (news, markets, weather, prices, scores).\n";
    out << "- Do NOT answer market/news/weather questions from memory or tell the user to check external websites.\n";
    out << "- Do NOT say \"check Yahoo Finance\", \"visit Bloomberg\", or similar — run web_search instead.\n";
    out << "- Call web_search when you are unsure and external lookup would help.\n";
    out << "- You may call web_search multiple times in one turn with different or refined queries when "
           "results are sparse, off-topic, or missing key facts.\n";
    out << "- Only emit your final blocks JSON reply when you have enough information and no further "
           "searches are needed.\n";
    out << "- Never put tool calls inside blocks JSON. Do not emit { \"tool\": ... } blocks.\n";
    out << "- Tools are invoked by the runtime via function calling, not via block types.\n";
    out << "- When results contain story titles, quote those headlines directly; do not list news homepages.\n";
    out << "- Never expose raw tool JSON to the user; summarize in plain blocks.\n\n";
  }

  out << ChatBlocksProfile() << "\n\n";
  out << "When you are ready to answer the user (no more tools needed), respond with exactly one ```json "
         "blocks fence.\n";
  out << "Example response:\n```json\n";
  out << R"({
  "blocks": [
    { "type": "heading", "level": 2, "text": "Overview" },
    { "type": "paragraph", "text": "Here are the supported block types." },
    { "type": "list", "ordered": false, "items": ["paragraph", "heading", "list", "code", "button"] },
    { "type": "button", "label": "Tell me more", "message": "Can you give an example of each block type?" },
    { "type": "code", "text": "function hello() {\n  return 42;\n}" }
  ]
})";
  out << "\n```\n";
  return out.str();
}

std::string PromptBuilder::FormatSearchResultsForLlm(const std::string& search_results_json) {
  nlohmann::json doc = nlohmann::json::parse(search_results_json, nullptr, false);
  if (doc.is_discarded() || !doc.contains("results") || !doc["results"].is_array()) {
    return search_results_json;
  }

  std::ostringstream out;
  int index = 1;
  for (const auto& result : doc["results"]) {
    if (!result.is_object()) {
      continue;
    }
    const std::string title = result.value("title", "");
    const std::string snippet = result.value("snippet", "");
    if (title.empty() && snippet.empty()) {
      continue;
    }
    out << index++ << ". ";
    if (!title.empty()) {
      out << title;
    }
    if (!snippet.empty() && snippet != title) {
      out << "\n   " << snippet;
    }
    out << '\n';
  }

  if (index == 1) {
    return "No usable search results.";
  }
  return out.str();
}

std::string PromptBuilder::BuildProactiveSearchContext(const std::string& query, const std::string& search_results) {
  std::ostringstream out;
  out << "PROACTIVE WEB SEARCH (already completed for this turn)\n";
  out << "Query: " << query << "\n";
  out << "Results:\n" << FormatSearchResultsForLlm(search_results) << "\n";
  out << "INSTRUCTIONS FOR THIS TURN\n";
  out << "- The runtime already ran an initial web_search. Use these results as your starting point.\n";
  out << "- If they lack the specific facts you need, call web_search again with a refined query before "
         "answering.\n";
  out << "- List specific headlines or facts from search titles/snippets in your blocks reply.\n";
  out << "- Do NOT list news outlets, brands, or homepages (CNN, FOX, Google News, NPR, etc.) as the answer.\n";
  out << "- Do NOT tell the user to visit websites, apps, or \"check\" external sources.\n";
  out << "- Skip results that are only generic site descriptions with no concrete story.\n";
  out << "- Use a list block with one item per real headline when possible.\n";
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
    { "type": "list", "ordered": false, "items": ["paragraph", "heading", "list", "code", "button"] },
    { "type": "button", "label": "Tell me more", "message": "Can you give an example of each block type?" },
    { "type": "code", "text": "function hello() {\n  return 42;\n}" }
  ]
})";
  out << "\n```\n";
  return out.str();
}

} // namespace pbr
