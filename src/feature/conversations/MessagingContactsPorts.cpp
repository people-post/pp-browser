#include "feature/conversations/MessagingContactsPorts.h"

#include "feature/conversations/ConversationsHub.h"
#include "common/PbrCompat.h"

namespace pbr {

MessagingContactsPorts MakeMessagingContactsPorts(ConversationsHub& hub) {
  MessagingContactsPorts ports;
  ports.snapshot = [&hub]() { return ProjectMessagingView(hub); };
  ports.list_contacts = [&hub]() { return hub.Contacts().List(); };
  ports.search_contacts = [&hub](const std::string& query) { return hub.Contacts().SearchLocal(query); };
  ports.get_contact = [&hub](const std::string& contact_id) { return hub.Contacts().Get(contact_id); };
  ports.upsert_contact = [&hub](const Contact& contact) { return hub.Contacts().Upsert(contact); };
  ports.add_empty_contact = [&hub]() { return hub.Contacts().AddEmpty(); };
  ports.remove_contact = [&hub](const std::string& contact_id) { return hub.Contacts().Remove(contact_id); };
  ports.list_threads = [&hub]() { return hub.Inbox().ListThreads(); };
  ports.sum_unread_for_contact = [&hub](const std::string& contact_id) {
    return hub.Inbox().SumUnreadForContact(contact_id);
  };
  ports.find_or_create_direct_thread = [&hub](const std::string& contact_id, const ThreadChannel channel) {
    return hub.Inbox().FindOrCreateDirectThread(contact_id, channel);
  };
  ports.notify_thread_changed = [&hub]() { hub.Inbox().NotifyThreadChanged(); };
  ports.get_signing_key = [&hub](const std::string& peer_identity_kind, const std::string& peer_identity_value) {
    return hub.SigningKeys().Get(peer_identity_kind, peer_identity_value);
  };
  ports.lookup_relay_user = [&hub](const std::string& relay_user_id) {
    return hub.Directory().LookupRelayUser(relay_user_id);
  };
  ports.contact_icon_local_path = [&hub](const Contact& contact) { return hub.ContactIconLocalPath(contact); };
  ports.ensure_contact_icon_cached = [&hub](const Contact& contact) { hub.EnsureContactIconCached(contact); };
  ports.ensure_directory_hit_icon_cached = [&hub](const DirectoryHit& hit) { hub.EnsureDirectoryHitIconCached(hit); };
  ports.apply_remote_snapshot = [&hub](const std::string& contact_id, const DirectoryHit& hit,
                                        const int64_t fetched_at_ms) {
    auto result = hub.Contacts().ApplyRemoteSnapshot(contact_id, hit, fetched_at_ms);
    if (result) {
      hub.EnsureContactIconCached(*result);
    }
    return result;
  };
  ports.register_contact_direct_endpoints = [&hub](const Contact& contact) {
    hub.MeshMessaging().RegisterContactDirectEndpoints(contact);
  };
  ports.warm_peer_for_thread = [&hub](const std::string& thread_id) { hub.MeshMessaging().WarmPeerForThread(thread_id); };
  ports.ensure_psk_generated = [&hub](const std::string& thread_id) {
    auto result = hub.MeshMessaging().EnsurePskGenerated(thread_id);
    if (!result) {
      return Roe<void>(result.error());
    }
    return Roe<void>();
  };
  ports.register_peer_signing_key = [&hub](const std::string& peer_identity_kind, const std::string& peer_identity_value,
                                           const std::string& signing_public_key_b64,
                                           const std::string& source) {
    hub.MeshMessaging().RegisterPeerSigningKey(peer_identity_kind, peer_identity_value, signing_public_key_b64, source);
  };
  ports.register_peer_kem_key = [&hub](const std::string& peer_identity_kind, const std::string& peer_identity_value,
                                        const std::string& kem_public_key_b64, const std::string& source) {
    hub.MeshMessaging().RegisterPeerKemKey(peer_identity_kind, peer_identity_value, kem_public_key_b64, source);
  };
  ports.is_contact_reachable = [&hub](const Contact& contact) { return hub.IsContactReachable(contact); };
  return ports;
}

} // namespace pbr
