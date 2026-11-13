#include "feature/conversations/RelayGrantPeers.h"

#include "lib/people/ContactIdentity.h"

#include <set>

namespace pbr {

RelayGrantPeers::RelayGrantPeers(ContactsStore& contacts, IGroupRosterStore& rosters, std::function<std::string()> self)
    : contacts_(contacts), rosters_(rosters), self_(std::move(self)) {}

bool RelayGrantPeers::InLiveGroup(const std::string& group_id) const {
  auto metadata = rosters_.LoadMetadata(group_id);
  if (!metadata || !*metadata) {
    return false;
  }
  return (*metadata)->sync_state != GroupSyncState::Removed && (*metadata)->sync_state != GroupSyncState::Dissolved;
}

bool RelayGrantPeers::Granted(const std::string& account_id) const {
  if (!IsAccountIdentityValue(account_id) || (self_ && account_id == self_())) {
    return false;
  }
  if (auto contact = contacts_.FindByIdentity(account_id, mesh::ContactIdKind::Account); contact && *contact) {
    return (*contact)->trust != TrustLevel::Blocked;
  }
  auto groups = rosters_.ListGroupIds();
  if (!groups) {
    return false;
  }
  for (const std::string& group_id : *groups) {
    if (auto in = rosters_.IsMember(group_id, account_id); in && *in && InLiveGroup(group_id)) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> RelayGrantPeers::All() const {
  const std::string self = self_ ? self_() : std::string();
  std::set<std::string> blocked;
  std::set<std::string> granted;
  if (auto contacts = contacts_.List()) {
    for (const Contact& contact : *contacts) {
      auto account = ContactAccountId(contact);
      if (!account || *account == self) {
        continue;
      }
      (contact.trust == TrustLevel::Blocked ? blocked : granted).insert(*account);
    }
  }
  if (auto groups = rosters_.ListGroupIds()) {
    for (const std::string& group_id : *groups) {
      if (!InLiveGroup(group_id)) {
        continue;
      }
      auto members = rosters_.ListMembers(group_id);
      if (!members) {
        continue;
      }
      for (const GroupRosterMember& member : *members) {
        if (IsAccountIdentityValue(member.member_identity) && member.member_identity != self &&
            !blocked.contains(member.member_identity)) {
          granted.insert(member.member_identity);
        }
      }
    }
  }
  return {granted.begin(), granted.end()};
}

std::vector<std::string> RelayGrantPeers::Blocked() const {
  std::vector<std::string> out;
  if (auto contacts = contacts_.List()) {
    for (const Contact& contact : *contacts) {
      if (auto account = ContactAccountId(contact); account && contact.trust == TrustLevel::Blocked) {
        out.push_back(*account);
      }
    }
  }
  return out;
}

} // namespace pbr
