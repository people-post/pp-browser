#pragma once

#include "common/thread/ThreadTypes.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/PeerDisplayLabel.h"
#include "common/MeshAlias.h"

#include <functional>
#include <optional>
#include <string>

namespace pbr {

/** What naming a peer needs beyond the contacts. */
struct PeerDisplaySources {
  /** A stranger's directory entry as cached (unverified; never a lookup); none when unknown. */
  std::function<std::optional<mesh::DirectoryHit>(const std::string& identity)> directory;
  /** A group's shared title (its roster metadata); none when unknown. */
  std::function<std::optional<std::string>(const std::string& group_id)> group_title;
};

/**
 * The name a thread or a sender shows: a contact's title, else the directory's unverified
 * nickname, else a short raw id; a group's local title over its shared one (kept as the subtitle).
 */
class PeerDisplayResolver {
public:
  PeerDisplayResolver(ContactsStore& contacts, PeerDisplaySources sources);

  PeerDisplayLabel ResolveThread(const Thread& thread) const;
  PeerDisplayLabel ResolveDirectPeer(const std::string& peer_identity_kind,
                                     const std::string& peer_identity_value) const;
  PeerDisplayLabel ResolveSender(const std::string& sender_contact_id) const;

private:
  ContactsStore& contacts_;
  PeerDisplaySources sources_;
};

} // namespace pbr
