#include "lib/people/ContactIdentity.h"
#include "common/MeshAlias.h"

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

std::optional<std::string> ContactAccountId(const Contact& contact) {
  const std::string value = PrimaryIdOfKind(contact, mesh::ContactIdKind::Account);
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::string> ContactRelayUserId(const Contact& contact) {
  const std::string value = PrimaryIdOfKind(contact, mesh::ContactIdKind::RelayUser);
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

std::optional<std::string> PrimaryRelayIdFromHit(const mesh::DirectoryHit& hit) {
  return PrimaryIdFromHit(hit, mesh::ContactIdKind::RelayUser);
}

} // namespace pbr
