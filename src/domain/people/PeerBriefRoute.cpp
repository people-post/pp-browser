#include "domain/people/PeerBriefRoute.h"

#include "domain/people/ContactIdentity.h"
#include "common/MeshAlias.h"

namespace pbr {

bool IsRelayUserIdValue(const std::string& value) {
  return value.rfind("relay:", 0) == 0;
}

std::optional<std::string> RelayUserIdFromContact(const Contact& contact) {
  for (const mesh::ContactId& id : contact.ids) {
    if (id.kind == mesh::ContactIdKind::RelayUser && !id.value.empty()) {
      return id.value;
    }
  }
  return std::nullopt;
}

std::optional<std::string> RelayUserIdFromDirectoryHit(const mesh::DirectoryHit& hit) {
  if (auto relay = PrimaryRelayIdFromHit(hit)) {
    return relay;
  }
  if (!hit.hit_id.empty() && IsRelayUserIdValue(hit.hit_id)) {
    return hit.hit_id;
  }
  return std::nullopt;
}

std::optional<std::string> ContactRelayRoute(const Thread& thread, ContactsStore& contacts) {
  // Communicating identity is Account ID; Brief delivery still needs relay: (M010).
  if (!thread.participant_contact_ids.empty()) {
    const auto contact = contacts.Get(thread.participant_contact_ids.front());
    if (contact && *contact) {
      if (auto relay = RelayUserIdFromContact(**contact)) {
        return relay;
      }
    }
  }
  if (IsAccountIdentityValue(thread.peer_identity_value)) {
    auto by_account = contacts.FindByIdentity(thread.peer_identity_value, mesh::ContactIdKind::Account);
    if (by_account && by_account->has_value()) {
      return RelayUserIdFromContact(**by_account);
    }
  }
  return std::nullopt;
}

std::optional<std::string> ResolvePeerBriefRoute(
    const Thread& thread, ContactsStore& contacts,
    const std::unordered_map<std::string, std::string>& account_to_relay) {
  if (auto relay = ContactRelayRoute(thread, contacts)) {
    return relay;
  }
  if (IsAccountIdentityValue(thread.peer_identity_value)) {
    const auto learned = account_to_relay.find(thread.peer_identity_value);
    if (learned != account_to_relay.end() && !learned->second.empty()) {
      return learned->second;
    }
  }

  return std::nullopt;
}

} // namespace pbr
