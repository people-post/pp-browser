#include "feature/ui/CallController.h"

#include "base/messaging/CallTypes.h"
#include "base/people/ContactTypes.h"
#include "base/platform/ILocalNotifier.h"
#include "base/ui/ShellTypes.h"
#include "feature/messaging/MessagingHub.h"
#include "feature/ui/ShellHost.h"
#include "feature/ui/UserFeedback.h"

#include "common/Utilities.h"

namespace pbr {

CallController& CallController::Instance() {
  static CallController instance;
  return instance;
}

void CallController::BindToMessaging() {
  if (bound_ || !MessagingHub::Instance().IsInitialized()) {
    return;
  }
  if (auto* calls = MessagingHub::Instance().Calls()) {
    calls->SetOnRingChanged([this]() { RefreshPendingRing(); });
    bound_ = true;
  }
}

void CallController::Tick() {
  BindToMessaging();
  if (auto* calls = MessagingHub::Instance().Calls()) {
    calls->SweepExpiredInvites();
  }
}

void CallController::OnCallWake() {
  RefreshPendingRing();
  if (!ringing_call_id_.empty()) {
    ILocalNotifier::Instance().NotifyIncoming("Incoming call", "Someone is calling you", "");
  }
}

void CallController::ClearRing() {
  ringing_call_id_.clear();
  ShellHost::Instance().State().call_ring = {};
}

void CallController::ClearInCall() {
  active_call_id_.clear();
  ShellHost::Instance().State().call_in_progress = {};
}

void CallController::SyncShellState() {
  ShellHost::Instance().DirtyWindow();
  ShellHost::Instance().RequestSyncLayout();
}

void CallController::RefreshPendingRing() {
  BindToMessaging();
  auto* calls = MessagingHub::Instance().Calls();
  if (!calls) {
    return;
  }

  if (auto active = calls->ActiveLocalCall(); active && active->has_value()) {
    active_call_id_ = (*active)->call_id;
    ClearRing();
    auto& in_call = ShellHost::Instance().State().call_in_progress;
    in_call.active = true;
    in_call.call_id = (*active)->call_id;
    in_call.title = (*active)->media_mode == CallMediaMode::Video ? "Video call" : "Voice call";
    in_call.subtitle = "Media not connected yet";
    SyncShellState();
    return;
  }
  ClearInCall();

  auto top = calls->TopPendingInvite();
  if (!top || !top->has_value()) {
    ClearRing();
    SyncShellState();
    return;
  }

  ringing_call_id_ = (*top)->call_id;
  auto& ring = ShellHost::Instance().State().call_ring;
  ring.active = true;
  ring.call_id = (*top)->call_id;
  ring.caller_label = (*top)->inviter_identity;
  if (auto contact = MessagingHub::Instance().Contacts().FindByIdentity((*top)->inviter_identity,
                                                                       ContactIdKind::RelayUser)) {
    if (*contact) {
      ring.caller_label =
          (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
      if (ring.caller_label.empty()) {
        ring.caller_label = (*top)->inviter_identity;
      }
    }
  }
  ring.media_label = (*top)->media_mode == CallMediaMode::Video ? "Video call" : "Voice call";
  SyncShellState();
}

bool CallController::StartCall(const std::string& thread_id, const bool video) {
  BindToMessaging();
  auto* calls = MessagingHub::Instance().Calls();
  if (!calls) {
    UserFeedback::Fail("Calls unavailable");
    return false;
  }
  auto thread = MessagingHub::Instance().Store().GetThread(thread_id);
  if (!thread || !*thread || (*thread)->kind != ThreadKind::Direct) {
    UserFeedback::Fail("Voice and video calls are available in 1:1 chats");
    return false;
  }
  if ((*thread)->peer_identity_value.empty()) {
    UserFeedback::Fail("Missing peer identity");
    return false;
  }
  auto started =
      calls->StartCall(thread_id, video ? CallMediaMode::Video : CallMediaMode::Voice, {(*thread)->peer_identity_value});
  if (!started) {
    UserFeedback::Fail(started.error().message);
    return false;
  }
  active_call_id_ = started->call_id;
  RefreshPendingRing();
  return true;
}

bool CallController::StartVoiceCall(const std::string& thread_id) {
  return StartCall(thread_id, false);
}

bool CallController::StartVideoCall(const std::string& thread_id) {
  return StartCall(thread_id, true);
}

void CallController::AcceptIncoming() {
  if (ringing_call_id_.empty()) {
    return;
  }
  auto* calls = MessagingHub::Instance().Calls();
  if (!calls) {
    return;
  }
  if (auto accepted = calls->AcceptInvite(ringing_call_id_); !accepted) {
    UserFeedback::Fail(accepted.error().message);
  }
  RefreshPendingRing();
}

void CallController::DeclineIncoming() {
  if (ringing_call_id_.empty()) {
    return;
  }
  auto* calls = MessagingHub::Instance().Calls();
  if (!calls) {
    return;
  }
  if (auto declined = calls->DeclineInvite(ringing_call_id_); !declined) {
    UserFeedback::Fail(declined.error().message);
  }
  RefreshPendingRing();
}

void CallController::LeaveActive() {
  if (active_call_id_.empty()) {
    return;
  }
  auto* calls = MessagingHub::Instance().Calls();
  if (!calls) {
    return;
  }
  if (auto left = calls->LeaveCall(active_call_id_); !left) {
    UserFeedback::Fail(left.error().message);
  }
  RefreshPendingRing();
}

void CallController::AcceptCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
  Instance().AcceptIncoming();
}

void CallController::DeclineCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
  Instance().DeclineIncoming();
}

void CallController::LeaveCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
  Instance().LeaveActive();
}

} // namespace pbr
