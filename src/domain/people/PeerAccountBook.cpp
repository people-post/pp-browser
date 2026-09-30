#include "domain/people/PeerAccountBook.h"

#include "domain/people/ContactIdentity.h"
#include "domain/people/ContactTypes.h"
#include "domain/people/MeshHopPolicy.h"

#include "common/PbrCompat.h"

namespace pbr {

PeerAccountBook::PeerAccountBook(ContactsStore& contacts) : contacts_(contacts) {
  redirectLogger("PeerAccountBook");
}

bool PeerAccountBook::Learn(const std::string& account, const std::string& peer_id) {
  if (account.empty() || peer_id.empty() || !IsAccountIdentityValue(account)) {
    return false;
  }
  account_by_peer_id_[peer_id] = account;
  LearnIntoContact(account, peer_id);
  return true;
}

void PeerAccountBook::LearnIntoContact(const std::string& account, const std::string& peer_id) {
  auto found = contacts_.FindByIdentity(account, ContactIdKind::Account);
  if (!found || !found->has_value()) {
    log().info << "PeerId learned map-only (no contact) peer_id=" << peer_id << " account=" << account;
    return;
  }
  Contact contact = **found;
  if (PeerIdFromContact(contact) == peer_id) {
    return;
  }
  for (const ContactId& id : contact.ids) {
    if (id.kind == ContactIdKind::PeerId && id.value == peer_id) {
      return;
    }
  }
  contact.ids.push_back(ContactId{ContactIdKind::PeerId, peer_id, false});
  contact.remote.ids = contact.ids;
  PromoteFlatFieldsToNested(contact);
  SyncContactMirrors(contact);
  if (auto saved = contacts_.Upsert(contact); !saved) {
    log().warning << "PeerId contact upsert failed account=" << account << " peer=" << peer_id
                  << " err=" << saved.error().message;
    return;
  }
  log().info << "PeerId learned into contact peer_id=" << peer_id << " account=" << account;
}

Roe<std::optional<std::string>> PeerAccountBook::PeerIdForAccount(const std::string& account) const {
  if (account.empty() || !IsAccountIdentityValue(account)) {
    return std::optional<std::string>{};
  }
  for (const auto& [peer_id, known] : account_by_peer_id_) {
    if (known == account && !peer_id.empty()) {
      return std::optional<std::string>{peer_id};
    }
  }
  auto found = contacts_.FindByIdentity(account, ContactIdKind::Account);
  if (!found) {
    return found.error();
  }
  if (found->has_value()) {
    if (std::string peer_id = PeerIdFromContact(**found); !peer_id.empty()) {
      return std::optional<std::string>{std::move(peer_id)};
    }
  }
  return std::optional<std::string>{};
}

Roe<std::optional<std::string>> PeerAccountBook::AccountForPeerId(const std::string& peer_id,
                                                                  const std::vector<std::string>& candidates) const {
  if (peer_id.empty()) {
    return std::optional<std::string>{};
  }
  if (const auto it = account_by_peer_id_.find(peer_id); it != account_by_peer_id_.end()) {
    return std::optional<std::string>{it->second};
  }
  for (const std::string& candidate : candidates) {
    if (candidate.empty()) {
      continue;
    }
    auto found = contacts_.FindByIdentity(candidate, ContactIdKind::Account);
    if (!found) {
      return found.error();
    }
    if (found->has_value() && PeerIdFromContact(**found) == peer_id) {
      return std::optional<std::string>{candidate};
    }
  }
  // Any contact with this PeerId (or a /p2p/ PeerId in its multiaddrs).
  auto listed = contacts_.List();
  if (!listed) {
    return listed.error();
  }
  for (const Contact& contact : *listed) {
    if (PeerIdFromContact(contact) != peer_id) {
      continue;
    }
    if (auto account = ContactAccountId(contact); account && !account->empty()) {
      return std::optional<std::string>{*account};
    }
  }
  return std::optional<std::string>{};
}

} // namespace pbr
