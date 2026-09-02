#pragma once

#include "domain/messaging/GroupRosterStore.h"
#include "common/thread/ThreadTypes.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/PeerDisplayLabel.h"
#include "feature/messaging/DirectoryShadowCache.h"

namespace pbr {

class PeerDisplayResolver {
public:
  PeerDisplayResolver(ContactsStore& contacts, DirectoryShadowCache& shadows,
                      GroupRosterStore* group_roster = nullptr);

  PeerDisplayLabel ResolveThread(const Thread& thread) const;
  PeerDisplayLabel ResolveDirectPeer(const std::string& peer_identity_kind,
                                     const std::string& peer_identity_value) const;
  PeerDisplayLabel ResolveSender(const std::string& sender_contact_id) const;

private:
  ContactsStore& contacts_;
  DirectoryShadowCache& shadows_;
  GroupRosterStore* group_roster_ = nullptr;
};

} // namespace pbr
