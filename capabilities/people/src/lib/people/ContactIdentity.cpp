#include "lib/people/ContactIdentity.h"
#include "common/MeshAlias.h"
#include "lib/people/AvatarGlyph.h"

namespace pbr {

bool IsAccountIdentityValue(const std::string& value) {
  return value.rfind("account:", 0) == 0;
}

std::string PrimaryIdOfKind(const Contact& contact, const mesh::ContactIdKind kind) {
  for (const mesh::ContactId& id : contact.ids) {
    if (id.kind == kind && id.primary && !id.value.empty()) {
      return id.value;
    }
  }
  for (const mesh::ContactId& id : contact.ids) {
    if (id.kind == kind && !id.value.empty()) {
      return id.value;
    }
  }
  return {};
}

std::string ContactAvatarStableId(const Contact& contact) {
  return AvatarStableId(PrimaryIdOfKind(contact, mesh::ContactIdKind::Account),
                        PrimaryIdOfKind(contact, mesh::ContactIdKind::PeerId), contact.id);
}

std::optional<std::string> ContactAccountId(const Contact& contact) {
  const std::string value = PrimaryIdOfKind(contact, mesh::ContactIdKind::Account);
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
}

namespace {

std::optional<std::string> PrimaryIdFromHit(const mesh::DirectoryHit& hit, const mesh::ContactIdKind kind) {
  for (const mesh::ContactId& id : hit.ids) {
    if (id.kind == kind && id.primary && !id.value.empty()) {
      return id.value;
    }
  }
  for (const mesh::ContactId& id : hit.ids) {
    if (id.kind == kind && !id.value.empty()) {
      return id.value;
    }
  }
  return std::nullopt;
}

} // namespace

std::optional<std::string> PrimaryAccountIdFromHit(const mesh::DirectoryHit& hit) {
  if (hit.account_id && !hit.account_id->empty()) {
    return *hit.account_id;
  }
  return PrimaryIdFromHit(hit, mesh::ContactIdKind::Account);
}

} // namespace pbr
