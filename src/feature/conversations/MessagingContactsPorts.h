#pragma once

#include "lib/messaging/PeerSigningKeyStore.h"
#include "common/thread/ThreadTypes.h"
#include "lib/net/OrgBackendClients.h"
#include "lib/people/ContactTypes.h"
#include "common/Error.h"
#include "feature/conversations/MessagingUiPorts.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * Contacts / directory / thread ports for UI presenters.
 * Application fills from ConversationsHub. Clear via BindContactsPorts({}).
 */
struct MessagingContactsPorts {
  std::function<MessagingView()> snapshot;

  std::function<Roe<std::vector<Contact>>()> list_contacts;
  std::function<Roe<std::vector<Contact>>(const std::string& query)> search_contacts;
  std::function<Roe<std::optional<Contact>>(const std::string& contact_id)> get_contact;
  std::function<Roe<Contact>(const Contact& contact)> upsert_contact;
  /** Directory search (exact id or ranked name search, self dropped) for "add contact". */
  std::function<Roe<std::vector<mesh::DirectoryHit>>(const std::string& query)> search_people;
  std::function<Roe<Contact>(const mesh::DirectoryHit& hit)> add_contact_from_directory_hit;
  std::function<Roe<bool>(const std::string& contact_id)> remove_contact;
  std::function<Roe<Contact>(const std::string& contact_id, const mesh::DirectoryHit& hit, int64_t fetched_at_ms)>
      apply_remote_snapshot;

  std::function<Roe<std::vector<Thread>>()> list_threads;
  std::function<int(const std::string& contact_id)> sum_unread_for_contact;
  std::function<Roe<Thread>(const std::string& contact_id, ThreadChannel channel)> find_or_create_direct_thread;
  std::function<void()> notify_thread_changed;

  std::function<std::optional<PeerSigningKeyRecord>(const std::string& peer_identity_kind,
                                                     const std::string& peer_identity_value)>
      get_signing_key;

  std::function<Roe<mesh::DirectoryHit>(const std::string& relay_user_id)> lookup_relay_user;

  std::function<std::string(const Contact& contact)> contact_icon_local_path;
  std::function<void(const Contact& contact)> ensure_contact_icon_cached;
  std::function<void(const mesh::DirectoryHit& hit)> ensure_directory_hit_icon_cached;

  std::function<void(const Contact& contact)> register_contact_direct_endpoints;
  std::function<void(const std::string& thread_id)> warm_peer_for_thread;
  std::function<Roe<void>(const std::string& thread_id)> ensure_psk_generated;
  /** Share a contact into a chat as a contact card. */
  std::function<Roe<void>(const std::string& thread_id, const std::string& contact_id)> share_contact;
  /** The keys a directory entry carries for its account (each kept only when it proves to be the account's). */
  std::function<void(const mesh::DirectoryHit& hit)> register_peer_keys_from_directory;

  std::function<bool(const Contact& contact)> is_contact_reachable;
};

class ConversationsHub;

MessagingContactsPorts MakeMessagingContactsPorts(ConversationsHub& hub);

} // namespace pbr
