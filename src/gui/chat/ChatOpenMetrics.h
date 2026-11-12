#pragma once

#include "lib/ui/ChatWidgetTypes.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pbr {

/**
 * The row draws a picture bubble: a picture clip with `<img class="chat-attachment-image">` (a video poster does not),
 * or a picture's placeholder until its preview is made (B203).
 */
inline bool IsPictureBubbleRow(const MessageDisplayRow& row) {
  const std::string_view rml(row.content_rml.data(), row.content_rml.size());
  return rml.find("class=\"chat-attachment-image\"") != std::string_view::npos ||
         rml.find(" chat-picture-placeholder\"") != std::string_view::npos;
}

/** How many of the rows are picture bubbles (the `pictures` field of chat.open / chat.window). */
inline size_t CountPictureBubbleRows(const std::vector<MessageDisplayRow>& rows) {
  size_t count = 0;
  for (const MessageDisplayRow& row : rows) {
    if (IsPictureBubbleRow(row)) {
      ++count;
    }
  }
  return count;
}

/** A chat picture still waiting for its preview (B203): its clip is the placeholder. */
inline bool IsPicturePlaceholderRow(const MessageDisplayRow& row) {
  const std::string_view rml(row.content_rml.data(), row.content_rml.size());
  return rml.find(" chat-picture-placeholder\"") != std::string_view::npos;
}

/** How many of the rows show a picture's placeholder (the `placeholders` field of chat.open). */
inline size_t CountPicturePlaceholderRows(const std::vector<MessageDisplayRow>& rows) {
  size_t count = 0;
  for (const MessageDisplayRow& row : rows) {
    if (IsPicturePlaceholderRow(row)) {
      ++count;
    }
  }
  return count;
}

/** The bytes (hex, `pic-hash`) of every picture placeholder in the rows, each once. */
inline std::vector<std::string> PicturePlaceholderHashes(const std::vector<MessageDisplayRow>& rows) {
  static constexpr std::string_view kClip = " chat-picture-placeholder\"";
  static constexpr std::string_view kHash = " pic-hash=\"";
  std::vector<std::string> out;
  for (const MessageDisplayRow& row : rows) {
    const std::string_view rml(row.content_rml.data(), row.content_rml.size());
    for (size_t at = rml.find(kClip); at != std::string_view::npos; at = rml.find(kClip, at + kClip.size())) {
      const size_t open_end = rml.find('>', at);
      const size_t hash_at = rml.find(kHash, at);
      if (open_end == std::string_view::npos || hash_at == std::string_view::npos || hash_at > open_end) {
        continue;
      }
      const size_t begin = hash_at + kHash.size();
      const size_t end = rml.find('"', begin);
      std::string hash(rml.substr(begin, end - begin));
      if (std::find(out.begin(), out.end(), hash) == out.end()) {
        out.push_back(std::move(hash));
      }
    }
  }
  return out;
}

/**
 * chat.previews (B203): the previews of the pictures an open drew as placeholders, from the open until the last one
 * is drawn (or the chat is left first: complete=0). `visible_ms`: until those on screen at the first settled layout
 * were drawn. `slow_frames`: frames of 50 ms or more meanwhile (UiFrameStats::SlowFrames).
 */
class ChatPreviewsProbe {
public:
  using Clock = std::chrono::steady_clock;
  struct Report {
    int64_t count = 0;
    int64_t failed = 0;
    int64_t ms = 0;
    int64_t visible_ms = -1; ///< -1: not known (left before the layout settled, or before they came)
    int64_t slow_frames = 0;
    bool complete = false;
  };

  /** An open drew `waiting` (hex) as placeholders. Nothing to wait for: not started. */
  void Start(std::vector<std::string> waiting, const Clock::time_point opened, const int64_t slow_frames_now) {
    *this = ChatPreviewsProbe{};
    if (waiting.empty()) {
      return;
    }
    active_ = true;
    count_ = static_cast<int64_t>(waiting.size());
    waiting_ = std::move(waiting);
    opened_ = opened;
    slow_frames_at_start_ = slow_frames_now;
  }
  bool Active() const { return active_; }
  bool VisibleKnown() const { return visible_known_; }
  /** The placeholders on screen once the open's layout settled (said once). */
  void SetVisible(const std::vector<std::string>& on_screen, const Clock::time_point now) {
    if (!active_ || visible_known_) {
      return;
    }
    visible_known_ = true;
    for (const std::string& hash : on_screen) {
      if (std::find(waiting_.begin(), waiting_.end(), hash) != waiting_.end() &&
          std::find(visible_.begin(), visible_.end(), hash) == visible_.end()) {
        visible_.push_back(hash);
      }
    }
    if (visible_.empty()) {
      visible_ms_ = Ms(now);
    }
  }
  /** `hash`'s preview was drawn (or cannot be made: `failed`). True when it was the open batch's last. */
  bool Done(const std::string& hash, const bool failed, const Clock::time_point now) {
    if (!active_) {
      return false;
    }
    const auto found = std::find(waiting_.begin(), waiting_.end(), hash);
    if (found == waiting_.end()) {
      return false;
    }
    waiting_.erase(found);
    failed_ += failed ? 1 : 0;
    if (const auto on_screen = std::find(visible_.begin(), visible_.end(), hash); on_screen != visible_.end()) {
      visible_.erase(on_screen);
      if (visible_.empty()) {
        visible_ms_ = Ms(now);
      }
    }
    return waiting_.empty();
  }
  /** The line's values; the probe stops. */
  Report Finish(const Clock::time_point now, const int64_t slow_frames_now) {
    Report report;
    report.count = count_;
    report.failed = failed_;
    report.ms = Ms(now);
    report.complete = waiting_.empty();
    report.visible_ms = visible_ms_.value_or(report.complete && !visible_known_ ? report.ms : -1);
    report.slow_frames = slow_frames_now - slow_frames_at_start_;
    active_ = false;
    return report;
  }

private:
  int64_t Ms(const Clock::time_point now) const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - opened_).count();
  }

  bool active_ = false;
  bool visible_known_ = false;
  int64_t count_ = 0;
  int64_t failed_ = 0;
  int64_t slow_frames_at_start_ = 0;
  Clock::time_point opened_{};
  std::vector<std::string> waiting_;
  std::vector<std::string> visible_;
  std::optional<int64_t> visible_ms_;
};

} // namespace pbr
