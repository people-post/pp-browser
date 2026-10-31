#pragma once

#include "feature/calls/CallStack.h"
#include "feature/conversations/HubParticipant.h"
#include "common/thread/ThreadTypes.h"

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Calls riding the hub: owns the call stack and drives it from the hub's lifecycle. The call
 * stack is built when a messaging stack is (never on Amp attach: that would abandon a call
 * already ringing over the relay) and borrows the hub's stores, delivery and mesh objects.
 */
class CallsParticipant : public HubParticipant {
public:
  CallStack& Stack() { return stack_; }
  /** Whether the active local call is this thread's (origin / peer / group). UI thread. */
  bool ThreadInActiveCall(const Thread& thread);
  /** Any call at all: one ringing here (an invite waiting) or one this device is in. Read only. UI thread. */
  bool AnyCallLive();
  Roe<std::vector<CallParticipant>> JoinedParticipants(const std::string& call_id);

  Roe<void> OnStoresOpened(ConversationsHub& hub) override;
  std::vector<IDekConsumer*> DekConsumers() override;
  void OnMessagingBuilt(ConversationsHub& hub) override;
  void OnPolicyPrefs(const HubPolicyPrefs& prefs) override;
  void OnTeardownStarting() override;
  void OnMessagingDropped() override;
  void OnShutdown() override;
  void OnMeshMediaDetach() override;
  void OnMeshServicesStarted(ConversationsHub& hub) override;
  void OnMeshMediaRebound(ConversationsHub& hub) override;
  void OnMeshStopping(const std::function<void()>& abort_circuits) override;
  void OnMeshStopped() override;
  void OnObservedAddressChanged() override;
  void OnLocalNetworkChanged(const NetworkChange& change, const mesh::LocalNetworkChange* local) override;
  void OnShutdownRequested() override;
  bool WantsEphemeralListen() const override;

private:
  CallStack stack_;
};

} // namespace pbr
