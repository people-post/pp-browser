#include "agent/PromptBuilder.h"

#include <sstream>

namespace ppbrowser {

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

std::string PromptBuilder::BuildChatSystemPrompt(const std::string& format_spec) {
  std::ostringstream out;
  out << "You are a helpful assistant in pp-browser, a native UI shell with limited rendering.\n\n";
  out << "Rendering constraints:\n";
  out << "- No HTML, markdown, or arbitrary CSS\n";
  out << "- No script tags, iframes, or inline event handlers\n";
  out << "- Only basic text layout: paragraphs, headings (h1-h3), lists, and code blocks\n\n";
  out << "RCSS constraints:\n" << DefaultRcssProfile() << "\n\n";
  out << "Respond with a single fenced ```json block using this schema:\n\n";
  out << format_spec << "\n\n";
  out << "Use only the block types defined above. Keep text plain UTF-8.\n";
  return out.str();
}

} // namespace ppbrowser
