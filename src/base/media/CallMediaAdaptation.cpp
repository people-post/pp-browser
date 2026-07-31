#include "base/media/CallMediaAdaptation.h"

#include <algorithm>

namespace pbr {

CallAdaptationDecision CallMediaAdaptation::Evaluate(const CallAdaptationInput& in) {
  CallAdaptationDecision out;
  out.publish_audio = !in.muted;
  out.target_audio_bps = kDefaultAudioBps;
  out.reason = "audio_priority";

  int64_t budget = in.per_user_up_bps;
  if (budget <= 0) {
    budget = kDefaultAudioBps + kDefaultVideoLoBps; // unbounded → treat as comfortable
  }

  const bool path_ok = in.path_pressure < 0.75;
  int64_t remaining = budget;
  if (out.publish_audio) {
    remaining -= out.target_audio_bps;
  }

  const bool want_video = in.camera_user_wants && path_ok && remaining >= kMinVideoLoBps;
  if (want_video) {
    out.publish_video_lo = true;
    out.target_video_lo_bps = std::min(kDefaultVideoLoBps, remaining);
    if (out.target_video_lo_bps < kMinVideoLoBps) {
      out.publish_video_lo = false;
      out.target_video_lo_bps = 0;
      out.reason = "uplink_too_low_for_video";
    } else {
      remaining -= out.target_video_lo_bps;
      out.reason = "audio_plus_video_lo";
    }
  } else if (in.camera_user_wants && !path_ok) {
    out.reason = "path_pressure_drop_video";
  } else if (in.camera_user_wants) {
    out.reason = "uplink_too_low_for_video";
  }

  if (in.allow_video_hi && out.publish_video_lo && path_ok && remaining >= kDefaultVideoHiBps / 2) {
    out.publish_video_hi = true;
    out.target_video_hi_bps = std::min(kDefaultVideoHiBps, remaining);
    out.reason = "audio_plus_lo_hi";
  }

  out.camera_allowed = out.publish_video_lo || out.publish_video_hi;
  return out;
}

const char* CallMediaAdaptation::ChannelTypeName(CallMediaRole role) {
  switch (role) {
  case CallMediaRole::Audio:
    return "reliable_ordered";
  case CallMediaRole::VideoLo:
  case CallMediaRole::VideoHi:
    return "latest_lossy";
  }
  return "best_effort";
}

bool CallMediaTopology::ShouldUseMediaRelay(size_t joined_count) {
  return joined_count >= 3;
}

bool CallMediaTopology::ShouldSoftMigrateToSfu(size_t previous_joined, size_t new_joined) {
  return previous_joined < 3 && new_joined >= 3;
}

CallMediaPathAction CallMediaTopology::DecidePath(size_t joined_count, bool has_sfu_hint) {
  if (joined_count >= 3) {
    return CallMediaPathAction::UseSfu;
  }
  if (has_sfu_hint) {
    return CallMediaPathAction::IgnoreSfuHint;
  }
  return CallMediaPathAction::StayP2p;
}

} // namespace pbr
