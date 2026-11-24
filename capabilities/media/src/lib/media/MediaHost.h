#pragma once

#include "lib/media/DeviceVitals.h"

#include <functional>
#include <optional>

namespace pbr {

/**
 * What media asks of the app that hosts it (projects/capability-repos CR023 step 5: the host contract). Media reaches
 * the OS itself (SDL's devices, the platform codecs, iOS's AVAudioSession, Android SDK calls through SDL's activity)
 * but never the app's own code. Where the OS work lives in the app — Android: the activity owns AudioManager's call
 * mode, communication device and audio focus, and the battery / thermal readers — media asks through these ports, and
 * the app tells it what happened through AudioSession's Notify*Host* entry points.
 *
 * Set once at startup (the app's main), before any call. An unset port answers as a host that cannot be asked yet: a
 * call to make reports "not done", a question has no answer. Platforms that ask nothing leave it empty. Ports may be
 * called from any thread (the engine's capture thread, the ringback's, the UI thread, the host's main thread); they
 * must not call back into AudioSession.
 */
struct MediaHost {
  /** The phone's call audio mode on / off (Android: MODE_IN_COMMUNICATION, the call's audio focus). False: not done. */
  std::function<bool(bool active)> set_call_audio_mode;
  /** The live call's speakerphone on / off (Android: its communication device). False: not done. */
  std::function<bool(bool on)> set_speakerphone;
  /** Whether the phone has an earpiece apart from the loudspeaker (most tablets do not). Nothing: cannot tell yet. */
  std::function<std::optional<bool>()> has_earpiece_route;
  /** Ask for the call's audio focus again after losing it for good: whether it was granted. Nothing: could not ask. */
  std::function<std::optional<bool>()> retake_call_audio_focus;
  /** The device's power and heat: low_power, thermal, battery_pct and charging (cpu_s is media's). Nothing: unknown. */
  std::function<std::optional<DeviceVitals>()> read_device_vitals;
};

/** Replaces the host's ports (the app, once at startup; tests). */
void SetMediaHost(MediaHost host);
/** The host's ports, as last set (empty ports before). Any thread. */
MediaHost CurrentMediaHost();

} // namespace pbr
