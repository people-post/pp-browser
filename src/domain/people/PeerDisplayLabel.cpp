#include "domain/people/PeerDisplayLabel.h"
#include "common/MeshAlias.h"

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

std::string FormatDirectoryTitle(const mesh::DirectoryHit& hit) {
  const std::string& nick = !hit.nickname.empty() ? hit.nickname : hit.display_name;
  std::string person_id;
  if (hit.account_id && !hit.account_id->empty()) {
    person_id = *hit.account_id;
  }
  if (person_id.empty()) {
    for (const mesh::ContactId& id : hit.ids) {
      if (id.kind == mesh::ContactIdKind::Account && !id.value.empty()) {
        person_id = id.value;
        break;
      }
    }
  }
  if (person_id.empty()) {
    for (const mesh::ContactId& id : hit.ids) {
      if (id.kind == mesh::ContactIdKind::RelayUser && !id.value.empty()) {
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

} // namespace pbr
