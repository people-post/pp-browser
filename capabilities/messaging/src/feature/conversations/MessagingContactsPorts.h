#pragma once

#include "lib/people/PeerSigningKeyStore.h"
#include "common/thread/ThreadTypes.h"
#include "lib/net/OrgBackendClients.h"
#include "lib/people/ContactTypes.h"
#include "common/Error.h"
#include "feature/conversations/ContactRefreshWorkflow.h"
#include "feature/conversations/MessagingUiPorts.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/** One row of the Groups page (Contacts → Groups, New chat → Groups): a group this account is in, with its chat. */
struct GroupListEntry {
  std::string thread_id;
  /** The name the chat list shows for it (InboxController::ResolveThreadLabel). */
  std::string title;
  /** The letter avatar's colour seed, as on the chat list. */
  std::string stable_id;
  size_t member_count = 0;
};

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

  std::function<Roe<std::vector<Thread>>()> list_threads;
  std::function<int(const std::string& contact_id)> sum_unread_for_contact;
  std::function<Roe<Thread>(const std::string& contact_id, ThreadChannel channel)> find_or_create_direct_thread;
  /** The same chat, found or made without opening it: not activated, unread count kept (a call's lookup). */
  std::function<Roe<Thread>(const std::string& contact_id, ThreadChannel channel)> find_direct_thread_quietly;
  std::function<void()> notify_thread_changed;

  std::function<std::optional<PeerSigningKeyRecord>(const std::string& peer_identity_kind,
                                                    const std::string& peer_identity_value)>
      get_signing_key;

  std::function<std::string(const Contact& contact)> contact_icon_local_path;
  std::function<void(const Contact& contact)> ensure_contact_icon_cached;
  std::function<void(const mesh::DirectoryHit& hit)> ensure_directory_hit_icon_cached;

  std::function<void(const Contact& contact)> register_contact_direct_endpoints;
  std::function<void(const std::string& thread_id)> warm_peer_for_thread;
  std::function<Roe<void>(const std::string& thread_id)> ensure_psk_generated;
  /**
   * Add the person a pasted invite names (a locator, ACCOUNT_CARD.md). `added` once, on the calling thread: the
   * contact's id as soon as it is stored, or why it was not. After a stored contact, `card` once the account's card
   * was asked for (its devices, its relay, the directory: seconds), with whether its keys are known now; on a worker or
   * I/O thread.
   */
  std::function<void(const std::string& text, std::function<void(Roe<std::string>)> added,
                     std::function<void(bool keys_known)> card)>
      add_from_locator;
  /** Share a contact into a chat as a contact card. */
  std::function<Roe<void>(const std::string& thread_id, const std::string& contact_id)> share_contact;
  /** B236: the chat sends nothing for now (the recipient has not added us; ConversationsFacade::ChatSendHeld). */
  std::function<bool(const std::string& thread_id)> send_held;
  /** The keys a directory entry carries for its account (each kept only when it proves to be the account's). */
  std::function<void(const mesh::DirectoryHit& hit)> register_peer_keys_from_directory;

  std::function<bool(const Contact& contact)> is_contact_reachable;
  /**
   * A contact whose account's card is still unknown: ask for it again (#409). `done(true)` once one is kept;
   * on any thread.
   */
  std::function<void(const Contact& contact, std::function<void(bool kept)> done)> refresh_contact_card;
  /** The contact's nickname and photo again when its last directory answer is stale; background, silent (B142). */
  std::function<void(const std::string& contact_id)> refresh_contact_profile;
  /**
   * The contact page's Refresh: its devices, then its relays, then ours, off the UI thread (ContactRefreshWorkflow).
   * `done` once, on the UI thread. False (no `done`) when one already runs for that contact.
   */
  std::function<bool(const std::string& contact_id, std::function<void(const ContactRefreshResult&)> done)>
      refresh_contact;
  /** The contact page's relay line: its preferred relay and what it answered at the last refresh (multi-relay R3). */
  std::function<ContactRelayView(const Contact& contact)> contact_relay;
  /** The groups this account is in now, each with its chat (ListLiveGroups): the Groups page. Unordered. */
  std::function<Roe<std::vector<GroupListEntry>>()> list_groups;
};

class ConversationsHub;

MessagingContactsPorts MakeMessagingContactsPorts(ConversationsHub& hub);

} // namespace pbr
