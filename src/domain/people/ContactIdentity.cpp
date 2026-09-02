#include "domain/people/ContactIdentity.h"

namespace pbr {

bool IsAccountIdentityValue(const std::string& value) {
  return value.rfind("account:", 0) == 0;
}

std::string PrimaryIdOfKind(const Contact& contact, const ContactIdKind kind) {
  for (const ContactId& id : contact.ids) {
    if (id.kind == kind && id.primary && !id.value.empty()) {
      return id.value;
    }
  }
  for (const ContactId& id : contact.ids) {
    if (id.kind == kind && !id.value.empty()) {
      return id.value;
    }
  }
  return {};
}

std::optional<std::string> ContactAccountId(const Contact& contact) {
  const std::string value = PrimaryIdOfKind(contact, ContactIdKind::Account);
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::string> ContactRelayUserId(const Contact& contact) {
  const std::string value = PrimaryIdOfKind(contact, ContactIdKind::RelayUser);
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
}

namespace {

std::optional<std::string> PrimaryIdFromHit(const DirectoryHit& hit, const ContactIdKind kind) {
  for (const ContactId& id : hit.ids) {
    if (id.kind == kind && id.primary && !id.value.empty()) {
      return id.value;
    }
  }
  for (const ContactId& id : hit.ids) {
    if (id.kind == kind && !id.value.empty()) {
      return id.value;
    }
  }
  return std::nullopt;
}

} // namespace

std::optional<std::string> PrimaryAccountIdFromHit(const DirectoryHit& hit) {
  if (hit.account_id && !hit.account_id->empty()) {
    return *hit.account_id;
  }
  return PrimaryIdFromHit(hit, ContactIdKind::Account);
}

std::optional<std::string> PrimaryRelayIdFromHit(const DirectoryHit& hit) {
  return PrimaryIdFromHit(hit, ContactIdKind::RelayUser);
}

} // namespace pbr
