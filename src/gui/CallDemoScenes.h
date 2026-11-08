#pragma once

#include "common/media/CallMediaHealth.h"
#include "lib/calls/CallScreen.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pbr {

/**
 * Made-up call screens for the UI demo app (kUiDemoBuild; it has no network peer): what the presenter and the media
 * health would report at `elapsed_ms` into a scripted call, so the real CallController / CallConnectStory /
 * ShellCallChromeRml path draws each call page on the simulator. Always compiled (covered by the normal tests); only
 * the demo app reads PP_DEMO_CALL ("voice", "voice@12" starts 12 s in) and calls it.
 *
 * Scenes: ring, ring_group, voice (calling → answered → relayed → a closer way at 25 s → slower at 35 s), reconnect,
 * video, failed, answer_group (a group ring answered at 3 s), group_voice, group_video, and V060's on hold:
 * voice_hold / video_hold (the other side has the 1:1 call on hold), group_hold (one member on hold, another's camera
 * paused). In the group scenes we speak
 * from 5 s, the first member from 11 s and the second from 20 s (the speaking rings and the spotlight); the 1:1 video
 * scene has both people in its roster, so its small picture swaps.
 */
struct DemoCallFrame {
  bool valid = false;
  CallScreen screen;
  std::string path_kind;
  CallPathQuality quality = CallPathQuality::Good;
  int quality_bars = 4;
  /** The video stage shows (the camera is on). */
  bool video_stage = false;
  /** Made-up audio levels (0..1): each member's by identity, and our microphone's. */
  std::vector<std::pair<std::string, float>> levels;
  float self_level = 0.f;
};

DemoCallFrame DemoCallSceneAt(std::string_view scene, int64_t elapsed_ms, int64_t now_ms);

/** "voice@12" → {"voice", 12000}. */
std::pair<std::string, int64_t> ParseDemoCallScene(std::string_view spec);

} // namespace pbr
