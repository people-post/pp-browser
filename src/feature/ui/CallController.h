#pragma once

#include "base/media/CallRingtone.h"
#include "feature/messaging/MessagingCallPorts.h"
#include "feature/ui/CallChromeSync.h"
#include "feature/ui/PeoplePickerNotifyPorts.h"
#include "feature/ui/ShellCallChromePorts.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

class CallMediaEngine;
class CallLifecycle;
class CallSessionManager;

/** Shell-level call ring / in-call chrome. */
class CallController {
public:
  CallController() = default;

  void BindCallPorts(MessagingCallPorts ports);
  /** Open people-picker flows without PeoplePickerController::Instance(). Clear via BindPeoplePickerNotify({}). */
  void BindPeoplePickerNotify(PeoplePickerNotifyPorts ports);
  /** Call ring / in-call chrome without ShellHost::Instance(). Clear via BindShellCallChrome({}). */
  void BindShellCallChrome(ShellCallChromePorts ports);

  void BindToMessaging();
  void Tick();
  /** After inbox sync / call_wake — fetch pending invites and show ring if needed. */
  void RefreshPendingRing();
  void OnCallWake();

  bool StartVoiceCall(const std::string& thread_id);
  bool StartVideoCall(const std::string& thread_id);
  /** Start with explicit invitee relay identities (group / picker flow). */
  bool StartCallWithInvitees(const std::string& thread_id, bool video,
                             const std::vector<std::string>& invitee_identities);
  void OpenGroupCallPicker(const std::string& thread_id, bool video);
  void OpenMidCallInvitePicker();
  void InviteIdentitiesToActiveCall(const std::vector<std::string>& invitee_identities);
  void AcceptIncoming();
  void DeclineIncoming();
  void LeaveActive();
  void RetryConnect();
  void ToggleMute();
  void ToggleCamera();

private:
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

  bool MessagingInitialized() const;
  CallSessionManager* Calls();
  CallLifecycle* Lifecycle();

  bool pending_call_wake_notify_ = false;
  /** Last CallSessionManager we installed OnRingChanged on (recreated across unlock). */
  const void* bound_calls_ = nullptr;
  std::string ringing_call_id_;
  std::string last_ring_call_id_;
  std::string active_call_id_;
  int64_t ring_started_ms_ = 0;
  int64_t last_pulse_toggle_ms_ = 0;
  int64_t last_ring_heartbeat_ms_ = 0;
  /** Last chrome applied — idle poll must not remount when unchanged. */
  CallChromeLayer synced_chrome_;
  CallRingtone ringtone_;
  MessagingCallPorts call_ports_;
  PeoplePickerNotifyPorts people_picker_notify_;
  ShellCallChromePorts shell_call_chrome_;
};

} // namespace pbr
