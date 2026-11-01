#include "feature/product/ProductStack.h"

#include "feature/conversations/ConversationsFacade.h"

#include "common/PbrCompat.h"

namespace pbr {

ProductStack::ProductStack() : hub_(std::make_unique<ConversationsHub>()), broadcast_(*hub_) {
  // Order matters: up in this order (calls, then broadcast), down in reverse.
  hub_->AddParticipant(calls_);
  hub_->AddParticipant(broadcast_);
}

ProductStack::~ProductStack() {
  hub_->Shutdown();
  hub_->ClearParticipants();
}

void ProductStack::BindFacade(ConversationsFacade& facade) {
  facade.SetActiveCallQuery([this](const Thread& thread) { return calls_.ThreadInActiveCall(thread); });
  facade.SetAnyCallQuery([this]() { return calls_.AnyCallLive(); });
}

MessagingPeoplePickerPorts ProductStack::PeoplePickerPorts() {
  MessagingPeoplePickerPorts ports = MakeMessagingPeoplePickerPorts(*hub_);
  ports.list_call_participants = [this](const std::string& call_id) { return calls_.JoinedParticipants(call_id); };
  return ports;
}

} // namespace pbr
