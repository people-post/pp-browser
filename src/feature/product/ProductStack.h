#pragma once

#include "feature/conversations/ConversationsHub.h"
#include "feature/conversations/MessagingPeoplePickerPorts.h"
#include "feature/product/BroadcastParticipant.h"
#include "feature/product/CallsParticipant.h"

#include <memory>
#include "common/PbrCompat.h"

namespace pbr {

class ConversationsFacade;

/**
 * The app's feature-level composition: the conversations hub (messaging + mesh host) with the
 * features that ride it — calls and live broadcast — registered as its participants. Conversations
 * never names calls or broadcast; this is where they meet.
 */
class ProductStack {
public:
  ProductStack();
  /** Shuts the hub down while the participants are still alive, then lets them go. */
  ~ProductStack();
  ProductStack(const ProductStack&) = delete;
  ProductStack& operator=(const ProductStack&) = delete;

  ConversationsHub& Hub() { return *hub_; }
  CallsParticipant& Calls() { return calls_; }
  BroadcastParticipant& Broadcast() { return broadcast_; }

  /** Let the facade answer call questions about threads. */
  void BindFacade(ConversationsFacade& facade);
  /** The people picker's ports, call participants included. */
  MessagingPeoplePickerPorts PeoplePickerPorts();

private:
  // The hub is declared first so it goes last: participants unbind from its registry on the way down.
  std::unique_ptr<ConversationsHub> hub_;
  CallsParticipant calls_;
  BroadcastParticipant broadcast_;
};

} // namespace pbr
