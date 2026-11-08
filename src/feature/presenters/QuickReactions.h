#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pbr {

/** The reactions every message sheet offers when the user has used none yet. */
const std::vector<std::string>& DefaultQuickReactions();

/** How many reactions the message sheet's row holds (it scrolls sideways past what fits). */
inline constexpr size_t kQuickReactionCount = 12;

/**
 * The reactions row above a message's actions (Kenneth 2026-11-04: "用户最常用的那些……可以左右滚动"): the emoji the
 * user used last first (`recent`, most recent first, as the profile keeps them), then the defaults not already
 * there, at most kQuickReactionCount. With nothing used yet it is the defaults.
 */
std::vector<std::string> QuickReactions(const std::vector<std::string>& recent);

} // namespace pbr
