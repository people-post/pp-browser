#pragma once

#include "common/media/CallMediaHealth.h"
#include "feature/presenters/CallMediaFrame.h"
#include "lib/calls/CallScreen.h"

#include <cstdint>
#include <optional>
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
 * Scenes: ring, ring_group, voice (calling → answered → relayed → a closer way at 25 s → slower at 35 s), reconnect
 * (down from 6 s, "They may be offline" from 26 s), reconnect_recovers (down 6-12 s, then "Restored"), video, failed,
 * answer_group (a group ring answered at 3 s), answer_voice (a 1:1 call from a tapped alert: waiting for the invite
 * 0-2 s, ringing 2-5 s, answered 5 s, connected at 10 s) and ring_awaiting (stays on the waiting page), group_voice, group_video, V060's on hold: voice_hold / video_hold (the
 * other side has the 1:1 call on hold), group_hold (one member on hold, another's camera paused), and V064's
 * ring_conflict (a call rings over the one this device is in) and call_back (the call taken ends at 8 s: "Call {name}
 * back?"). In the group scenes we speak from 5 s, the first member from 11 s and the second from 20 s (the speaking
 * rings and the spotlight); the 1:1 video scene has both people in its roster, so its small picture swaps.
 */
struct DemoCallFrame {
  bool valid = false;
  /** The tapped-alert page ("Incoming call", the three lines on their way): drawn by RenderAwaitingRing, no screen. */
  bool awaiting_invite = false;
  CallScreen screen;
  std::string path_kind;
  CallPathQuality quality = CallPathQuality::Good;
  int quality_bars = 4;
  /** The video stage shows (the camera is on). */
  bool video_stage = false;
  /** Made-up audio levels (0..1): each member's by identity, and our microphone's. */
  std::vector<std::pair<std::string, float>> levels;
  float self_level = 0.f;
  /** V064 (call_back): the call taken is over; this one, ended by End & accept, is offered back. */
  std::optional<CallScreen::EndingCall> call_back;
};

DemoCallFrame DemoCallSceneAt(std::string_view scene, int64_t elapsed_ms, int64_t now_ms);

/** What the engine and the call stack would report for the frame's in-call screen (CallMediaFrame's input). */
CallMediaFacts DemoCallMediaFacts(const DemoCallFrame& frame, int64_t now_ms);
/** The frame's made-up media health (its path, quality and bars). */
CallMediaHealthView DemoCallHealth(const DemoCallFrame& frame);

/** "voice@12" → {"voice", 12000}. */
std::pair<std::string, int64_t> ParseDemoCallScene(std::string_view spec);

} // namespace pbr
