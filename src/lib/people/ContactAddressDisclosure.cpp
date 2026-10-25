#include "lib/people/ContactAddressDisclosure.h"

#include "lib/people/MeshHopPolicy.h"

#include <string>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

mesh::AddressDisclosurePolicy BuildAddressDisclosurePolicy(const mesh::DirectAudience audience,
                                                     const std::vector<Contact>& contacts,
                                                     const std::vector<std::string>& own_devices) {
  mesh::AddressDisclosurePolicy policy;
  policy.audience = audience;
  for (const std::string& peer_id : own_devices) {
    if (!peer_id.empty()) {
      policy.contacts.insert(peer_id);
      policy.friendly.insert(peer_id);
    }
  }
  for (const Contact& contact : contacts) {
    std::vector<std::string> keys = PeerIdsFromContact(contact);
    for (const mesh::ContactId& id : contact.ids) {
      keys.push_back(id.value);
    }
    for (const std::string& key : keys) {
      if (key.empty()) {
        continue;
      }
      if (contact.trust == TrustLevel::Blocked) {
        policy.blocked.insert(key);
        continue;
      }
      policy.contacts.insert(key);
      if (contact.trust == TrustLevel::Friendly) {
        policy.friendly.insert(key);
      }
    }
  }
  return policy;
}

} // namespace pbr
