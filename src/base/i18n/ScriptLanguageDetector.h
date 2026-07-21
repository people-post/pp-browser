#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace pbr {

/** Infer a BCP-47 tag for chat message shaping (HarfBuzz locl). Empty when Latin-only. */
std::optional<std::string> DetectChatMessageLanguage(std::string_view utf8_text);

/** Insert `lang="…"` on the opening tag when detection succeeds. */
std::string ApplyLangAttribute(std::string_view opening_tag, std::string_view utf8_text);

} // namespace pbr
