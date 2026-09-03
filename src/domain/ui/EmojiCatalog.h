#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/** Curated in-app emoji catalog (not full Unicode). Categories match mobile pickers. */
struct EmojiCategory {
  std::string id;
  std::string label;
  std::string rail_glyph; // tab icon in the side rail
  std::vector<std::string> glyphs;
};

class EmojiCatalog {
public:
  static constexpr std::string_view kRecentlyUsedId = "recently_used";
  static constexpr std::size_t kRecentCap = 36;

  /** Static categories excluding recently_used (caller prepends recent). */
  static const std::vector<EmojiCategory>& StaticCategories();

  /** Build display categories: optional recent section first when non-empty. */
  static std::vector<EmojiCategory> BuildWithRecent(const std::vector<std::string>& recent_emojis);

  /** MRU update: move glyph to front, cap length, normalize empty away. */
  static void TouchRecent(std::vector<std::string>& recent, const std::string& glyph);
};

} // namespace pbr
