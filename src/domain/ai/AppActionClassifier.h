#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/**
 * On-device judgement "is this sentence a request to operate the app?" (no model, no I/O). Returns the PP tool
 * that fits, or nullopt, in which case the sentence goes to brief_AI as before. Only tools in `declared_tools`
 * can be returned; the first declared tool wins, phrase hits before verb+object hits.
 *
 * Two conditions, both required: the wordlist (AppActionWords.h) hits the tool, and the sentence has the form of
 * a request (imperative, about the user's own app/data, or a how-to). That form check is what keeps news and
 * third-party sentences out ("Did the US contact Iran about talks?"). When in doubt it says no.
 *
 * Chinese (any CJK character present) is a port of brief_AI backend/tools/app_actions.py and must stay equal
 * to it; anything else is judged by the English rules.
 */
std::optional<std::string> ClassifyAppAction(std::string_view message, const std::vector<std::string>& declared_tools);

} // namespace pbr
