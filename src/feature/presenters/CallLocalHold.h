#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "feature/presenters/CallHoldMute.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * This device's side of a call on hold (V060). While the OS has the call's audio (`AudioSession::IsInterrupted`) the
 * call is on hold: the others are told (`on_hold` in this device's roster row), the mic is muted (CallHoldMute) and the
 * camera rests; the hold's end undoes what the hold did and nothing the user chose meanwhile.
 *
 * Also the camera's whole timing (B225): the user's wish (`wants_`) is the one truth the button shows; the engine is
 * only asked for the difference, and only OPENING is gated. A close takes effect at once, whoever asks for it. An
 * open waits (a) kCameraSettleMs after our last close and (b) after a real background (SDL's DID_ENTER_BACKGROUND), for
 * a real return (WILL_ENTER_FOREGROUND) and then kCameraSettleMs of staying active. Only "inactive" (lock screen
 * flicker, Control Center, a banner, Face ID) moves nothing. The device is never opened in the background. All
 * times are monotonic (`mono_ms`); the wall clock plays no part.
 *
 * Telling the others needs the call's media (a call still ringing out has none yet): until it has been told, the
 * hold is sent again each tick (at most once a second) until it is, or the hold ends first and there is nothing to say.
 *
 * UI thread only: the GUI hands it the OS's facts each tick and renders its notices; it acts through `Ports`.
 */
class CallLocalHold : public Module {
public:
  struct Ports {
    /** The call's mic is muted right now (false without a call). */
    std::function<bool()> mic_muted;
    /**
     * The engine's camera is on right now. Only the read-back safety net reads it (an engine that closed the camera
     * silently); never the wish, never what the request in flight will make it (the request is asynchronous).
     */
    std::function<bool()> engine_camera_on;
    /**
     * Tell the call's others (CallUiBackend::SetLocalOnHold); `done` on the UI thread. `mute`, when set, is the mic
     * change the hold makes: it is applied together with the hold, so one roster leaves carrying both (B221).
     */
    std::function<void(bool on_hold, std::optional<bool> mute, std::function<void(Roe<void>)> done)> publish_hold;
    std::function<void(bool muted)> set_mic_muted;
    /**
     * CallUiBackend::SetLocalVideoEnabled: the device and the roster together, `video_paused` cleared either way.
     * Every (re)opening goes through it; `done` on the UI thread.
     */
    std::function<void(bool enabled, std::function<void(Roe<void>)> done)> set_camera_enabled;
    /** CallUiBackend::SetLocalVideoPaused(true): the rest (background, hold) — the one thing that says "paused". */
    std::function<void()> pause_camera;
    /** A camera the user asked for was refused by the call (no media, video not allowed, device error): say so. */
    std::function<void()> camera_open_failed;
    /** A one-line notice for the user (an i18n key). */
    std::function<void(const std::string& key)> note;
  };
  /** What a tick knows. */
  struct Facts {
    /** The call this device is in (empty: none). */
    std::string call_id;
    /** The OS has this device's call audio (AudioSession::IsInterrupted). */
    bool audio_taken = false;
    /** The app is active now / its screen is up (AppLifecycle::IsForeground / IsVisible): only where a call starts. */
    bool foreground = true;
    bool visible = true;
    /**
     * An open request is sent only while the app is active (iOS: it delivers will-enter-background first and a lone
     * "active" in the middle of it). Android may deliver will-enter-background without a later did-enter-foreground,
     * so there the camera keeps opening without it.
     */
    bool open_needs_active = true;
    /** Wall clock: the publish retry only. */
    int64_t now_ms = 0;
    /** Monotonic: every gate and settle time. */
    int64_t mono_ms = 0;
  };
  /** A hold the call could not be told yet goes again after this long. */
  static constexpr int64_t kPublishRetryMs = 1000;
  /**
   * The camera opens no sooner than this after our last close, and after a real return only once the app has stayed
   * active this long: ONE constant for both (Kenneth 2026-11-14: 2-3 s; the later words win over 5-10 s).
   */
  static constexpr int64_t kCameraSettleMs = 2500;
  /** An open request that the engine still does not show after this long was not simply in flight: it closed itself. */
  static constexpr int64_t kEngineReadBackMs = 3000;

  explicit CallLocalHold(Ports ports);

  /** Every tick. */
  void Sync(const Facts& facts);
  /**
   * The app went to the background proper (SDL DID_ENTER_BACKGROUND), called from its callback: the camera rests now
   * (the one reliable execution point in the background), and nothing opens until a real return has settled.
   */
  void OnEnteredBackground(int64_t mono_ms);
  /** On its way back from the background proper (SDL WILL_ENTER_FOREGROUND): the next active is a real return. */
  void OnWillEnterForeground();
  /** The OS resigned the app active (lock screen flicker, Control Center, a banner, Face ID): moves nothing. */
  void OnInactive();
  /** The app is active again (SDL DID_ENTER_FOREGROUND, "active" only). */
  void OnActive(int64_t mono_ms);
  /**
   * The mute button (it then toggles as usual). During the hold the choice is the user's from here: the hold's end
   * changes nothing (never a locked button — if the OS never says the hold ended, the user can unmute by hand).
   */
  void OnMuteTapped();
  /**
   * The camera button: toggles what it shows (during a hold's rest it shows off, and the tap takes the rest off and
   * wants the camera). While the app is active but the state is not settled (the real-return signal never came), a tap
   * is "resume", not "off".
   */
  void OnCameraTapped(int64_t mono_ms);
  /** A video call's automatic camera-on at setup is the user's intent: the wish is set, the device waits for the gate. */
  void OnAutoCameraOn(int64_t mono_ms);
  /**
   * The engine closed the camera itself (permission denied, open failed, a give-up path that closed a device that was
   * streaming): the wish goes with it, and a reopen waits kCameraSettleMs from now like after any close.
   */
  void OnCameraClosedByEngine(int64_t mono_ms);
  /** We closed it by policy (narrowed to voice): our close, the gate counts from now. */
  void OnCameraClosedByUs(int64_t mono_ms);

  bool OnHold() const { return on_hold_; }
  /**
   * What the camera button shows (not the engine, which moves later): the user's wish, except while the camera only
   * rests for a hold, when it shows off (a tap there brings it back, like any tap on an off button).
   */
  bool WantsCameraOn() const { return wants_ && !(on_hold_ && !hold_lifted_); }
  /** Wanted but not on (waiting for the gate / the settle, or resting in the background): "Restoring video…". */
  bool CameraResuming() const { return wants_ && !asked_on_ && !(on_hold_ && !hold_lifted_); }

private:
  void BeginCall(const Facts& facts);
  /** Once the hold's own mute is seen applied (it is asynchronous), an open mic means the user unmuted. */
  void WatchUserUnmute();
  void Transition(bool held, bool in_call, int64_t now_ms, int64_t mono_ms);
  void PublishIfOwed(bool in_call, int64_t now_ms);
  /** The one rule: ask the engine for the difference between what is wanted now and what was last asked. */
  void Reconcile(int64_t mono_ms);
  void SetWanted(int64_t mono_ms, bool user);
  bool Settled(int64_t mono_ms) const { return !bg_seen_ && mono_ms >= settle_until_; }
  /** The tap that resumes: the real-return signal was lost, the app is active and the user is looking at it. */
  void ResumeByTap(int64_t mono_ms);
  void ReadBack(int64_t mono_ms);

  static constexpr int64_t kNever = INT64_MAX;
  /** "Long ago" for the gate (not INT64_MIN: the difference must not overflow). */
  static constexpr int64_t kLongAgo = -1'000'000'000'000'000;

  Ports ports_;
  std::string call_id_;
  /** Names the call the state below is about: a publish's answer for an earlier call is dropped. */
  uint64_t call_gen_ = 0;
  bool on_hold_ = false;
  /** What the call's others were last told (this call: nothing yet, so not on hold). */
  bool told_on_hold_ = false;
  bool publishing_ = false;
  int64_t next_publish_ms_ = 0;
  bool hold_mute_seen_applied_ = false;
  /** The hold's mic change, riding on the hold's publish (set and taken within one Transition). */
  std::optional<bool> pending_mute_;
  CallHoldMute hold_mute_;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);

  // The camera (all UI thread, all monotonic).
  /** The user wants the camera: the button shows it. */
  bool wants_ = false;
  /** What we last asked the engine: on (true) or off (false). The engine's own state is never read back as this. */
  bool asked_on_ = false;
  /** When we last asked it (an open still in flight until kEngineReadBackMs has passed). */
  int64_t asked_mono_ = kLongAgo;
  /** When it was last closed, by us or by the engine (the 2.5 s gate for the next open). */
  int64_t closed_mono_ = kLongAgo;
  /** We told the others "paused" and have not told them anything since: a later close must clear it. */
  bool paused_sent_ = false;
  /**
   * 0: settled; kNever: in the background, or inactive and not yet active again, or a call begun while the app was not
   * shown and not yet active; else when the settle ends.
   */
  int64_t settle_until_ = 0;
  /** A real background was seen and no real return yet. */
  bool bg_seen_ = false;
  /** The app is active (the read-back runs only then; on iOS no open is sent otherwise). */
  bool active_ = true;
  /** Facts::open_needs_active as of the last tick. */
  bool open_needs_active_ = true;
  /** During a hold the user tapped the camera: the hold no longer rests it. Cleared when the hold begins / ends. */
  bool hold_lifted_ = false;
  /** The wish was set by a tap (a refused open is then said to the user), not by the automatic start. */
  bool open_by_user_ = false;
  /** Names the open request in flight: an answer for an older one is dropped. */
  uint64_t request_seq_ = 0;
};

} // namespace pbr
