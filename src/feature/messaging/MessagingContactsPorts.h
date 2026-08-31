#pragma once

#include "base/messaging/PeerSigningKeyStore.h"
#include "base/messaging/ThreadTypes.h"
#include "base/net/ServiceClients.h"
#include "base/people/ContactTypes.h"
#include "common/Error.h"
#include "feature/messaging/MessagingUiPorts.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Contacts / directory / thread ports for UI presenters.
 * Application fills from MessagingHub. Clear via BindContactsPorts({}).
 */
struct MessagingContactsPorts {
  std::function<MessagingView()> snapshot;

  std::function<Roe<std::vector<Contact>>()> list_contacts;
  std::function<Roe<std::vector<Contact>>(const std::string& query)> search_contacts;
  std::function<Roe<std::optional<Contact>>(const std::string& contact_id)> get_contact;
  std::function<Roe<Contact>(const Contact& contact)> upsert_contact;
  std::function<Roe<Contact>()> add_empty_contact;
  std::function<Roe<bool>(const std::string& contact_id)> remove_contact;
  std::function<Roe<Contact>(const std::string& contact_id, const DirectoryHit& hit, int64_t fetched_at_ms)>
      apply_remote_snapshot;

  std::function<Roe<std::vector<Thread>>()> list_threads;
  std::function<int(const std::string& contact_id)> sum_unread_for_contact;
  std::function<Roe<Thread>(const std::string& contact_id, ThreadChannel channel)> find_or_create_direct_thread;
  std::function<void()> notify_thread_changed;

  std::function<std::optional<PeerSigningKeyRecord>(const std::string& peer_identity_kind,
                                                     const std::string& peer_identity_value)>
      get_signing_key;

  std::function<Roe<DirectoryHit>(const std::string& relay_user_id)> lookup_relay_user;

  std::function<std::string(const Contact& contact)> contact_icon_local_path;
  std::function<void(const Contact& contact)> ensure_contact_icon_cached;
  std::function<void(const DirectoryHit& hit)> ensure_directory_hit_icon_cached;

  std::function<void(const Contact& contact)> register_contact_direct_endpoints;
  std::function<void(const std::string& thread_id)> warm_peer_for_thread;
  std::function<Roe<void>(const std::string& thread_id)> ensure_psk_generated;
  std::function<void(const std::string& peer_identity_kind, const std::string& peer_identity_value,
                     const std::string& signing_public_key_b64, const std::string& source)>
      register_peer_signing_key;
  std::function<void(const std::string& peer_identity_kind, const std::string& peer_identity_value,
                     const std::string& kem_public_key_b64, const std::string& source)>
      register_peer_kem_key;

  std::function<bool(const Contact& contact)> is_contact_reachable;
};

class MessagingHub;

MessagingContactsPorts MakeMessagingContactsPorts(MessagingHub& hub);

} // namespace pbr
