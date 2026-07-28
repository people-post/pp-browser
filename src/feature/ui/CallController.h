#pragma once

#include "feature/ui/CallChromeSync.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>

#include <string>

namespace Rml {
class Context;
}

namespace pbr {

/** Shell-level call ring / in-call chrome (a1; no media). */
class CallController {
public:
  static CallController& Instance();

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

  static void AcceptCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
  static void DeclineCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
  static void LeaveCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);

private:
  CallController() = default;
  bool StartCall(const std::string& thread_id, bool video);
  void SyncShellState();
  void ClearRing();
  void ClearInCall();

  bool bound_ = false;
  std::string ringing_call_id_;
  std::string active_call_id_;
  /** Last chrome applied — idle poll must not remount when unchanged. */
  CallChromeLayer synced_chrome_;
};

} // namespace pbr
