#include "feature/presenters/ThreadSearchSession.h"

#include <utility>

namespace pbr {

uint64_t ThreadSearchSession::Open() {
  Close();
  view_.open = true;
  view_.indexing = true;
  building_ = true;
  return generation_;
}

uint64_t ThreadSearchSession::Close() {
  ++generation_;
  view_ = View{};
  building_ = false;
  rebuild_pending_ = false;
  last_built_ms_.reset();
  index_.clear();
  index_.shrink_to_fit();
  hits_.clear();
  hits_.shrink_to_fit();
  return generation_;
}

bool ThreadSearchSession::IndexReady(const uint64_t generation, std::vector<ThreadSearchEntry> index,
                                     const int64_t now_ms) {
  if (!view_.open || generation != generation_) {
    return false;
  }
  index_ = std::move(index);
  building_ = false;
  last_built_ms_ = now_ms;
  view_.indexing = false;
  Recompute(/*keep_cursor=*/true);
  return true;
}

bool ThreadSearchSession::IndexFailed(const uint64_t generation, const int64_t now_ms) {
  if (!view_.open || generation != generation_) {
    return false;
  }
  building_ = false;
  last_built_ms_ = now_ms; // the next try keeps the same spacing as a rebuild
  view_.indexing = false;
  return true;
}

void ThreadSearchSession::SetQuery(std::string query) {
  if (!view_.open || query == view_.query) {
    return;
  }
  view_.query = std::move(query);
  Recompute(/*keep_cursor=*/false);
  // A new query always shows its newest hit, even when that is the message already in view.
  view_.jump_to.reset();
  if (!hits_.empty()) {
    view_.jump_to = hits_[view_.current].message_id;
  }
}

void ThreadSearchSession::Older() {
  if (view_.current + 1 < hits_.size()) {
    ++view_.current;
    view_.jump_to = hits_[view_.current].message_id;
    RefreshFlags();
  }
}

void ThreadSearchSession::Newer() {
  if (view_.current > 0 && view_.current < hits_.size()) {
    --view_.current;
    view_.jump_to = hits_[view_.current].message_id;
    RefreshFlags();
  }
}

bool ThreadSearchSession::DropHit(const std::string& message_id) {
  size_t at = hits_.size();
  for (size_t i = 0; i < hits_.size(); ++i) {
    if (hits_[i].message_id == message_id) {
      at = i;
      break;
    }
  }
  if (at == hits_.size()) {
    return false;
  }
  hits_.erase(hits_.begin() + static_cast<std::ptrdiff_t>(at));
  if (at < view_.current) {
    --view_.current;
  } else if (at == view_.current) {
    // The cursor moves on to the next older hit (the newest, when it was the oldest).
    if (view_.current >= hits_.size()) {
      view_.current = hits_.empty() ? 0 : hits_.size() - 1;
    }
    view_.jump_to.reset();
    if (!hits_.empty()) {
      view_.jump_to = hits_[view_.current].message_id;
    }
  }
  RefreshFlags();
  return true;
}

std::optional<uint64_t> ThreadSearchSession::MessagesChanged(const int64_t now_ms) {
  if (!view_.open) {
    return std::nullopt;
  }
  rebuild_pending_ = true;
  return Tick(now_ms);
}

std::optional<uint64_t> ThreadSearchSession::Tick(const int64_t now_ms) {
  if (!view_.open || !rebuild_pending_ || building_) {
    return std::nullopt;
  }
  if (last_built_ms_ && now_ms - *last_built_ms_ < kRebuildGapMs) {
    return std::nullopt;
  }
  rebuild_pending_ = false;
  building_ = true;
  return ++generation_;
}

std::optional<std::string> ThreadSearchSession::TakeJump() {
  std::optional<std::string> jump = std::move(view_.jump_to);
  view_.jump_to.reset();
  return jump;
}

std::string ThreadSearchSession::CurrentMessageId() const {
  return view_.current < hits_.size() ? hits_[view_.current].message_id : std::string{};
}

void ThreadSearchSession::Recompute(const bool keep_cursor) {
  const std::string before = CurrentMessageId();
  std::vector<ThreadSearchHit> found = FindHits(index_, view_.query);
  view_.current = keep_cursor ? RealignCurrent(hits_, view_.current, found) : 0;
  hits_ = std::move(found);
  const std::string after = CurrentMessageId();
  if (after != before) {
    view_.jump_to.reset();
    if (!after.empty()) {
      view_.jump_to = after;
    }
  }
  RefreshFlags();
}

void ThreadSearchSession::RefreshFlags() {
  view_.hit_count = hits_.size();
  view_.can_older = view_.current + 1 < hits_.size();
  view_.can_newer = view_.current > 0 && !hits_.empty();
}

} // namespace pbr
