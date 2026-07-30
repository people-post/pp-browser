#pragma once

#include "base/media/CallRingtone.h"
#include "feature/ui/CallChromeSync.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <string>

namespace Rml {
class Context;
}

namespace pbr {

class MessagingHub;

class CallMediaEngine;

/** Shell-level call ring / in-call chrome. */
class CallController {
public:
  static CallController& Instance();

  void BindMessaging(MessagingHub& messaging);
  MessagingHub& Hub();
  const MessagingHub& Hub() const;

  void BindToMessaging();
  void Tick();
  /** After inbox sync / call_wake — fetch pending invites and show ring if needed. */
  void RefreshPendingRing();
  void OnCallWake();

  bool StartVoiceCall(const std::string& thread_id);
  bool StartVideoCall(const std::string& thread_id);
  void AcceptIncoming();
  void DeclineIncoming();
  void LeaveActive();
  void ToggleMute();
  void ToggleCamera();

  static void AcceptCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
  static void DeclineCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
  static void LeaveCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
  static void MuteCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
  static void CameraCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);

private:
  CallController() = default;
  bool StartCall(const std::string& thread_id, bool video);
  void SyncShellState();
  void ClearRing();
  void ClearInCall();
  /** Hide in-call bar without clearing active_call_id_ (conflict ring). */
  void HideInCallChrome();
  void ApplyAudioLevels(CallMediaEngine& media);
  void RefreshCallLevels();
  void SyncRingtone();
  std::string DisplayNameForIdentity(const std::string& identity) const;
  static std::string FormatElapsed(int64_t connected_at_ms);

  bool pending_call_wake_notify_ = false;
  /** Last CallSessionManager we installed OnRingChanged on (recreated across unlock). */
  const void* bound_calls_ = nullptr;
  std::string ringing_call_id_;
  std::string active_call_id_;
  int64_t ring_started_ms_ = 0;
  int64_t last_pulse_toggle_ms_ = 0;
  /** Last chrome applied — idle poll must not remount when unchanged. */
  CallChromeLayer synced_chrome_;
  CallRingtone ringtone_;
  MessagingHub* messaging_ = nullptr;

};

} // namespace pbr
