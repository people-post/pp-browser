#pragma once

#include "common/media/MediaRelayStream.h"
#include "lib/calls/CallScreen.h"
#include "lib/people/AvatarGlyph.h"
#include "lib/ui/CallConflictCopy.h"
#include "lib/ui/ShellTypes.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pbr {

/**
 * The call members' names and avatar paths, kept for one call (DEV/PLAN-call-frame-cost.md step 1). The in-call page is
 * rebuilt every frame, and each avatar path is a contact lookup, file checks and a JSON read (ProfileIconCache): on an
 * iPhone 11 the roster cost 216 ms of UI thread per 10 s in a 1:1 video call. A value is read again every kRecheckMs (no
 * notice of "contact renamed" / "photo downloaded" reaches the call screen), an empty one too (no photo file yet, the
 * "Stranger" placeholder: a member without a photo would otherwise be read from disk every frame). The members' call
 * state (muted, video, on hold, paused) is never kept here: FillCallRoster takes it from the screen every time.
 */
class CallRosterCache {
public:
  static constexpr int64_t kRecheckMs = 5000;

  struct Lookups {
    std::function<CallerName(const std::string& identity)> name;
    std::function<std::string(const std::string& identity)> icon;
  };

  /** The call on screen: a different one starts from nothing. */
  void ForCall(const std::string& call_id) {
    if (call_id != call_id_) {
      Clear();
      call_id_ = call_id;
    }
  }
  void Clear() {
    entries_.clear();
    call_id_.clear();
  }
  /** Identities with something kept. */
  size_t Size() const { return entries_.size(); }

  CallerName Name(const std::string& identity, const int64_t now_ms, const Lookups& lookups) {
    if (identity.empty() || !lookups.name) {
      return MakeCallerName({});
    }
    Entry& e = entries_[identity];
    if (!e.name_at || now_ms - *e.name_at >= kRecheckMs) {
      e.name = lookups.name(identity);
      e.name_at = now_ms;
    }
    return e.name;
  }

  std::string Icon(const std::string& identity, const int64_t now_ms, const Lookups& lookups) {
    if (identity.empty() || !lookups.icon) {
      return {};
    }
    Entry& e = entries_[identity];
    if (!e.icon_at || now_ms - *e.icon_at >= kRecheckMs) {
      e.icon = lookups.icon(identity);
      e.icon_at = now_ms;
    }
    return e.icon;
  }

private:
  struct Entry {
    CallerName name;
    std::optional<int64_t> name_at; // read at (unset: not read yet)
    std::string icon;
    std::optional<int64_t> icon_at;
  };

  std::string call_id_;
  std::unordered_map<std::string, Entry> entries_;
};

/**
 * The in-call roster rows: names and avatars through `cache`, every call state field from `members` (the screen's,
 * this frame). Our own row is called `you_label` and its name is never looked up.
 */
inline void FillCallRoster(std::vector<CallRosterParticipantState>& roster,
                           const std::vector<CallScreen::Member>& members, const std::string& you_label,
                           CallRosterCache& cache, const int64_t now_ms, const CallRosterCache::Lookups& lookups) {
  roster.clear();
  roster.reserve(members.size());
  for (const CallScreen::Member& member : members) {
    CallRosterParticipantState entry;
    entry.is_local = member.is_local;
    const CallerName caller = member.is_local ? CallerName{} : cache.Name(member.identity, now_ms, lookups);
    const std::string& name = member.is_local ? you_label : caller.label;
    entry.name = name.c_str();
    entry.identity = member.identity.c_str();
    if (const std::string avatar_path = cache.Icon(member.identity, now_ms, lookups); !avatar_path.empty()) {
      entry.has_avatar = true;
      entry.avatar_src = avatar_path.c_str();
    }
    entry.avatar_unknown = caller.unknown && !entry.has_avatar;
    const AvatarGlyph glyph = MakeAvatarGlyph(name, member.identity);
    entry.avatar_letter = glyph.letter.c_str();
    entry.avatar_tone = glyph.tone;
    entry.stream_id = std::to_string(PublisherStreamIdForIdentity(member.identity)).c_str();
    entry.audio_muted = member.audio_muted;
    entry.video_enabled = member.video_enabled;
    entry.on_hold = member.on_hold;
    entry.video_paused = member.video_paused;
    entry.guest = member.guest;
    roster.push_back(std::move(entry));
  }
}

} // namespace pbr
