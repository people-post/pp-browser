#include "gui/CallDemoScenes.h"

#include "foundation/i18n/LocalizationService.h"

#include <cstdlib>

namespace pbr {

namespace {

// DemoDataSeeder's contacts (their account ids are "demo-acct-" + contact id) and its first group.
constexpr const char* kPeer = "demo-acct-demo-contact-03";
constexpr const char* kGroupThread = "demo-thread-group-1";
constexpr const char* kSelf = "demo-acct-self";

using Status = CallScreen::Status;

CallScreen InCall(const char* call_id, const bool video, const bool group) {
  CallScreen s;
  s.kind = CallScreen::Kind::InCall;
  s.call_id = call_id;
  s.video = video;
  s.group = group;
  s.show_speaker = true;
  s.show_invite = true;
  if (group) {
    s.thread_id = kGroupThread;
    for (const char* who : {"demo-acct-demo-contact-01", "demo-acct-demo-contact-02", "demo-acct-demo-contact-07"}) {
      s.members.push_back({.identity = who, .video_enabled = video});
    }
    s.members[2].audio_muted = true;
    s.members[2].video_enabled = false;
    s.members.push_back({.identity = kSelf, .is_local = true, .video_enabled = video});
  } else {
    s.peer_identity = kPeer;
    s.placed_here = true;  // the 1:1 scenes are calls this device placed
    if (video) {
      s.members.push_back({.identity = kPeer, .video_enabled = true});
      s.members.push_back({.identity = kSelf, .is_local = true, .video_enabled = true});
    }
  }
  return s;
}

void Connected(CallScreen& s, const int64_t connected_at_ms) {
  s.status = Status::Connected;
  s.media_connected = true;
  s.was_connected = true;
  s.connected_at_ms = connected_at_ms;
}

} // namespace

std::pair<std::string, int64_t> ParseDemoCallScene(const std::string_view spec) {
  const size_t at = spec.find('@');
  if (at == std::string_view::npos) {
    return {std::string(spec), 0};
  }
  return {std::string(spec.substr(0, at)), std::atoll(std::string(spec.substr(at + 1)).c_str()) * 1000};
}

DemoCallFrame DemoCallSceneAt(const std::string_view scene, const int64_t t, const int64_t now_ms) {
  DemoCallFrame f;
  f.valid = true;
  const int64_t start = now_ms - t;
  if (scene == "ring" || scene == "ring_group") {
    f.screen.kind = CallScreen::Kind::Ringing;
    f.screen.ring_call_id = "demo-ring";
    const bool group = scene == "ring_group";
    f.screen.caller_identity = group ? "demo-acct-demo-contact-01" : kPeer;
    f.screen.ring_video = !group;
    f.screen.ring_group = group;
    f.screen.thread_id = group ? kGroupThread : "";
    return f;
  }
  if (scene == "voice" || scene == "video") {
    const bool video = scene == "video";
    f.screen = InCall(video ? "demo-video" : "demo-voice", video, false);
    f.video_stage = video;
    if (t < 4'000) {
      f.screen.status = Status::Calling;
    } else if (t < 7'000) {
      f.screen.status = Status::Connecting;
    } else {
      Connected(f.screen, start + 7'000);
      // Relayed at first; the direct retry finds a nearer way at 25 s (voice only); slower for a while at 35 s.
      f.path_kind = video || t < 25'000 ? (video ? "direct" : "circuit") : "punched";
      if (t >= 35'000 && t < 50'000) {
        f.quality = CallPathQuality::Fair;
        f.quality_bars = 2;
      }
    }
    return f;
  }
  if (scene == "reconnect") {
    f.screen = InCall("demo-reconnect", false, false);
    Connected(f.screen, start - 60'000);
    f.path_kind = "direct";
    if (t >= 6'000) {
      f.screen.status = Status::Reconnecting;
      f.screen.media_connected = false;
      f.quality = CallPathQuality::Reconnecting;
      f.quality_bars = 0;
    }
    return f;
  }
  if (scene == "voice_hold" || scene == "video_hold" || scene == "group_hold") {
    // V060, connected a minute ago: the other side has the 1:1 call on hold; in the group one member is on hold and
    // another's camera is paused (their app in the background).
    const bool group = scene == "group_hold";
    const bool video = scene != "voice_hold";
    f.screen = InCall(group ? "demo-group-hold" : video ? "demo-video-hold" : "demo-voice-hold", video, group);
    Connected(f.screen, start - 60'000);
    f.path_kind = group ? "media_relay" : "direct";
    f.video_stage = video && !group;  // as in group_video: the group page draws its own tiles
    if (group) {
      f.screen.members[0].on_hold = true;
      f.screen.members[1].video_paused = true;
      f.screen.members[1].video_enabled = false;
    } else {
      if (!video) {
        f.screen.members.push_back({.identity = kPeer});
        f.screen.members.push_back({.identity = kSelf, .is_local = true});
      }
      f.screen.members[0].on_hold = true;
    }
    return f;
  }
  if (scene == "failed") {
    f.screen = InCall("demo-failed", false, false);
    f.screen.status = Status::CouldntConnect;
    f.screen.show_retry = true;
    f.screen.show_invite = false;
    f.screen.hint = CallScreen::Hint::DirectConnect;
    f.screen.hint_seed_unreachable = true;
    return f;
  }
  if (scene == "answer_group") {
    // A group's ring, answered at 3 s: the page must go straight to the group's grid (no 1:1 page in between).
    if (t < 3'000) {
      f.screen.kind = CallScreen::Kind::Ringing;
    } else if (t < 6'000) {
      f.screen.kind = CallScreen::Kind::Answering;
      f.screen.show_speaker = true;
    } else {
      f.screen = InCall("demo-group-answer", false, true);
      if (t < 9'000) {
        f.screen.status = Status::SettingUpGroup;
      } else {
        Connected(f.screen, start + 9'000);
        f.path_kind = "media_relay";
      }
      return f;
    }
    f.screen.ring_call_id = "demo-group-answer";
    f.screen.caller_identity = "demo-acct-demo-contact-01";
    f.screen.ring_group = true;
    f.screen.thread_id = kGroupThread;
    return f;
  }
  if (scene == "group_voice" || scene == "group_video") {
    const bool video = scene == "group_video";
    f.screen = InCall(video ? "demo-group-video" : "demo-group-voice", video, true);
    if (t < 5'000) {
      f.screen.status = Status::SettingUpGroup;
    } else {
      Connected(f.screen, start + 5'000);
      f.path_kind = "media_relay";
      if (!video && t >= 30'000 && t < 40'000) {
        // The stack moving the group onto another path: a reconnect (the capsule says so; the line keeps the timer).
        f.screen.status = Status::Reconnecting;
      }
      if (video && t >= 15'000) {
        f.quality = CallPathQuality::Poor;
        f.quality_bars = 1;
      }
      // Who speaks: we (5-11 s), then the first member (11-20 s), then the second (20-30 s).
      f.self_level = t < 11'000 ? 0.2f : 0.f;
      f.levels = {{"demo-acct-demo-contact-01", t >= 11'000 && t < 20'000 ? 0.25f : 0.f},
                  {"demo-acct-demo-contact-02", t >= 20'000 && t < 30'000 ? 0.25f : 0.f}};
    }
    return f;
  }
  return {};
}

} // namespace pbr
