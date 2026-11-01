#pragma once

#include "feature/conversations/ConversationsHub.h"
#include "feature/product/MessagingPeoplePickerPorts.h"
#include "feature/product/BroadcastParticipant.h"
#include "feature/product/CallsParticipant.h"
#include "feature/product/ProductMesh.h"

#include <memory>
#include "common/PbrCompat.h"

namespace pbr {

class ConversationsFacade;

/**
 * The app's feature-level composition: the mesh (ProductMesh, built for the profile's role), the
 * conversations hub on it, and the features that ride the hub — calls and live broadcast —
 * registered as its participants. Conversations never names calls, broadcast or the node role;
 * this is where they meet.
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
  // The mesh goes after the hub (the hub's shutdown gives it back); the hub after its participants,
  // which unbind from its registry on the way down.
  ProductMesh mesh_;
  std::unique_ptr<ConversationsHub> hub_;
  CallsParticipant calls_;
  BroadcastParticipant broadcast_;
};

} // namespace pbr
