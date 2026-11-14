#pragma once

#include "common/PbrCompat.h"

namespace pbr {

/**
 * What the call shows right now, as far as the screen's sleep goes (B215). Filled each frame from the facts the call
 * screen already has; no SDL, no GUI.
 */
struct CallScreenAwakeFacts {
  /** A call is on this device's screen and not being left. */
  bool has_call = false;
  /** The call failed or was lost (couldn't connect / disconnected): its page may stay up, but it is not a call. */
  bool failed = false;
  /** Past "calling / connecting": the call is live. */
  bool connected = false;
  /** This is a video call (its mode), not a voice call. */
  bool video_mode = false;
  /** This device's camera is on (a ringing-out call's preview too). */
  bool camera_on = false;
  /** A peer's picture is actually received and shown (a group: at least one member's tile). */
  bool remote_video = false;
  /** This device has the call on hold. */
  bool on_hold = false;
  /** An incoming call is ringing on this device (also a second call ringing over a live one): its page is looked at. */
  bool ringing = false;
  /** This device started the call (calling out), as opposed to answering or joining it. */
  bool placed_here = false;
};

/**
 * The screen stays on only while a video picture is up: a live call with our camera or a peer's video showing, or a
 * video call we placed that has not connected yet (the camera preview is on screen). A voice call, a video call with
 * both cameras off, a call on hold, and no call let the screen sleep.
 */
inline bool CallWantsScreenOn(const CallScreenAwakeFacts& facts) {
  if (!facts.has_call || facts.failed || facts.on_hold) {
    return false;
  }
  if (facts.connected) {
    return facts.camera_on || facts.remote_video;
  }
  return facts.video_mode && facts.camera_on;
}

/**
 * The screen goes black against the face (the phone's proximity sensor) only in a voice call: one that is live, or one
 * this device placed that is not yet connected (the phone is at the ear while it rings out). Not while an incoming call
 * rings (the user is looking for the answer button), not in a video call (cameras off or not), not for a failed call's
 * page, not while the screen is held on for video. A call on hold still counts. Whether the sound comes from the
 * earpiece (not the speaker, headphones or Bluetooth) is the platform layer's decision from the real output route.
 */
inline bool CallWantsProximityScreenOff(const CallScreenAwakeFacts& facts) {
  if (!facts.has_call || facts.failed || facts.video_mode || facts.ringing || CallWantsScreenOn(facts)) {
    return false;
  }
  return facts.connected || facts.placed_here;
}

/** Why the screen is (not) held, for the metric: video_call / ended / hold / no_video. */
inline const char* CallScreenAwakeReason(const CallScreenAwakeFacts& facts) {
  if (CallWantsScreenOn(facts)) {
    return "video_call";
  }
  if (!facts.has_call || facts.failed) {
    return "ended";
  }
  return facts.on_hold ? "hold" : "no_video";
}

} // namespace pbr
