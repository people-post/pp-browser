#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace pbr {

/**
 * Unsent text per conversation (and one for Home): the composer is one widget, so its text is swapped when the
 * open conversation changes. Without this, a draft typed for A showed up in B and one Enter sent it to B.
 * In memory only (lost on quit).
 */
class ThreadDrafts {
public:
  static constexpr const char* kHome = "\x01home"; // never a thread id

  /**
   * The composer now belongs to `key`: keeps `current_text` for the conversation it belonged to and returns the
   * text saved for `key` (empty when none). The first call only records the owner.
   */
  std::string SwitchTo(const std::string& key, const std::string& current_text) {
    if (owner_ && *owner_ == key) {
      return current_text;
    }
    if (owner_) {
      Put(*owner_, current_text);
    }
    owner_ = key;
    auto it = drafts_.find(key);
    if (it == drafts_.end()) {
      return {};
    }
    std::string text = std::move(it->second);
    drafts_.erase(it);
    return text;
  }

  /** The conversation whose text is in the composer now (none before the first SwitchTo). */
  const std::optional<std::string>& Owner() const { return owner_; }

  /** Saves `text` for a conversation that is not open (empty text removes it). */
  void Put(const std::string& key, const std::string& text) {
    if (text.empty()) {
      drafts_.erase(key);
    } else {
      drafts_[key] = text;
    }
  }

  /** A send that could not go out goes back in front of what that conversation already had. */
  void Prepend(const std::string& key, const std::string& text) {
    auto it = drafts_.find(key);
    Put(key, it == drafts_.end() || it->second.empty() ? text : text + "\n" + it->second);
  }

  std::string Peek(const std::string& key) const {
    auto it = drafts_.find(key);
    return it == drafts_.end() ? std::string() : it->second;
  }

  /** The conversation was closed or cleared: its unsent text goes with it. */
  void Forget(const std::string& key) { drafts_.erase(key); }

private:
  std::unordered_map<std::string, std::string> drafts_;
  std::optional<std::string> owner_;
};

} // namespace pbr
