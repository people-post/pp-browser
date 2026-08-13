#include "base/people/PeerDisplayLabel.h"

namespace pbr {

std::string FormatContactTitle(const Contact& contact) {
  const std::string title = ContactEffectiveTitle(contact);
  if (!title.empty()) {
    return title;
  }
  if (!contact.display_name.empty()) {
    return contact.display_name;
  }
  return contact.server_nickname;
}

std::string FormatDirectoryTitle(const DirectoryHit& hit) {
  const std::string& nick = !hit.nickname.empty() ? hit.nickname : hit.display_name;
  std::string person_id;
  if (hit.account_id && !hit.account_id->empty()) {
    person_id = *hit.account_id;
  }
  if (person_id.empty()) {
    for (const ContactId& id : hit.ids) {
      if (id.kind == ContactIdKind::Account && !id.value.empty()) {
        person_id = id.value;
        break;
      }
    }
  }
  if (person_id.empty()) {
    for (const ContactId& id : hit.ids) {
      if (id.kind == ContactIdKind::RelayUser && !id.value.empty()) {
        person_id = id.value;
        break;
      }
    }
  }
  if (person_id.empty()) {
    person_id = hit.hit_id;
  }

  std::string title;
  if (!nick.empty()) {
    title = "~" + nick;
  }
  const std::string short_id = ShortRelayId(person_id);
  if (!short_id.empty()) {
    if (!title.empty()) {
      title += " ";
    }
    title += "@" + short_id;
  }
  return title;
}

std::string ShortRelayId(const std::string& relay_or_peer_id) {
  if (relay_or_peer_id.empty()) {
    return {};
  }
  constexpr size_t kKeep = 12;
  if (relay_or_peer_id.size() <= kKeep + 1) {
    return relay_or_peer_id;
  }
  // Prefer shortening after "relay:" prefix when present.
  constexpr const char* kPrefix = "relay:";
  constexpr size_t kPrefixLen = 6;
  if (relay_or_peer_id.compare(0, kPrefixLen, kPrefix) == 0) {
    const std::string rest = relay_or_peer_id.substr(kPrefixLen);
    if (rest.size() <= 8) {
      return relay_or_peer_id;
    }
    return std::string(kPrefix) + rest.substr(0, 8) + "…";
  }
  return relay_or_peer_id.substr(0, kKeep) + "…";
}

} // namespace pbr
