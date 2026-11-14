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
 * camera rests; the hold's end undoes what the hold did and nothing the user chose meanwhile. Also the camera's rest
 * while the app is in the background, which shares the camera's paused state with the hold.
 *
 * Telling the others needs the call's media (a call still ringing out has none yet): until it has been told, the
 * hold is sent again each tick (at most once a second) until it is, or the hold ends first and there is nothing to say.
 *
 * UI thread only: the GUI hands it the OS's facts each tick and renders its notices; it acts through `Ports`.
 */
class CallLocalHold : public Module {
public:
  struct Ports {
    /** The call's mic is muted / its camera is on right now (false without a call). */
    std::function<bool()> mic_muted;
    std::function<bool()> camera_on;
    /**
     * Tell the call's others (CallUiBackend::SetLocalOnHold); `done` on the UI thread. `mute`, when set, is the mic
     * change the hold makes: it is applied together with the hold, so one roster leaves carrying both (B221).
     */
    std::function<void(bool on_hold, std::optional<bool> mute, std::function<void(Roe<void>)> done)> publish_hold;
    std::function<void(bool muted)> set_mic_muted;
    std::function<void(bool paused)> set_camera_paused;
    /** A one-line notice for the user (an i18n key). */
    std::function<void(const std::string& key)> note;
  };
  /** What a tick knows. */
  struct Facts {
    /** The call this device is in (empty: none). */
    std::string call_id;
    /** The OS has this device's call audio (AudioSession::IsInterrupted). */
    bool audio_taken = false;
    bool foreground = true;
    int64_t now_ms = 0;
  };
  /** A hold the call could not be told yet goes again after this long. */
  static constexpr int64_t kPublishRetryMs = 1000;

  explicit CallLocalHold(Ports ports);

  /** Every tick. */
  void Sync(const Facts& facts);
  /** Mobile, in a call: the OS stops the camera in the background — the call pauses it and says so (video_paused). */
  void OnAppBackground();
  void OnAppForeground();
  /**
   * The mute button (it then toggles as usual). During the hold the choice is the user's from here: the hold's end
   * changes nothing (never a locked button — if the OS never says the hold ended, the user can unmute by hand).
   */
  void OnMuteTapped();
  /** The camera button. True when the tap only took the hold's rest off (the camera is back; nothing else to do). */
  bool TakeCameraTap();

  bool OnHold() const { return on_hold_; }
  /** The camera rests (on hold, or the app in the background); it still counts as on. */
  bool CameraPaused() const { return camera_paused_; }

private:
  void BeginCall(const std::string& call_id);
  /** Once the hold's own mute is seen applied (it is asynchronous), an open mic means the user unmuted. */
  void WatchUserUnmute();
  void Transition(bool held, bool in_call, bool foreground, int64_t now_ms);
  void PublishIfOwed(bool in_call, int64_t now_ms);

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
  bool camera_paused_ = false;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace pbr
