#include "feature/presenters/QuickReactions.h"

#include "common/EmojiKey.h"

#include <algorithm>

namespace pbr {

const std::vector<std::string>& DefaultQuickReactions() {
  static const std::vector<std::string> defaults = {"👍", "❤️", "😂", "😮", "😢", "🙏"};
  return defaults;
}

std::vector<std::string> QuickReactions(const std::vector<std::string>& recent) {
  std::vector<std::string> row;
  std::vector<std::string> keys;
  const auto add = [&](const std::string& emoji) {
    const std::string key = NormalizeEmojiKey(emoji);
    if (key.empty() || row.size() >= kQuickReactionCount ||
        std::find(keys.begin(), keys.end(), key) != keys.end()) {
      return;
    }
    keys.push_back(key);
    row.push_back(emoji);
  };
  for (const std::string& emoji : recent) {
    add(emoji);
  }
  for (const std::string& emoji : DefaultQuickReactions()) {
    add(emoji);
  }
  return row;
}

} // namespace pbr
