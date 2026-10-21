#pragma once

#include <string>
#include <string_view>

namespace pbr {

/**
 * Text that is about to become inner RML (SetInnerRML, data-rml, a serialized view). Markup characters
 * become entities, and so do braces: inner RML goes through the data-binding pass, where a literal `{{…}}`
 * typed by a person would otherwise be evaluated as an expression. Prefer binding plain text with
 * `{{expr}}` in the template, which sets text and needs no escaping; use this where markup is built.
 */
inline std::string EscapeRml(const std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    case '{':
      out += "&#123;";
      break;
    case '}':
      out += "&#125;";
      break;
    default:
      out += c;
      break;
    }
  }
  return out;
}

} // namespace pbr
