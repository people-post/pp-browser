#include "domain/people/ContactAddressDisclosure.h"

#include "domain/people/MeshHopPolicy.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

AddressDisclosurePolicy BuildAddressDisclosurePolicy(const DirectAudience audience,
                                                     const std::vector<Contact>& contacts) {
  AddressDisclosurePolicy policy;
  policy.audience = audience;
  for (const Contact& contact : contacts) {
    std::vector<std::string> keys = PeerIdsFromContact(contact);
    for (const ContactId& id : contact.ids) {
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
