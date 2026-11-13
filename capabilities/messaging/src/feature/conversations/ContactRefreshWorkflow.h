#pragma once

#include "domain/peers/AccountKeyResolver.h"
#include "feature/conversations/ContactCardExchange.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/PeerCards.h"
#include "mesh/types/IDirectoryClient.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/** What a contact refresh came to: which sources answered and what changed (the page says it in words). */
struct ContactRefreshResult {
  enum class Devices {
    NotAsked,   // no Account ID or no device to ask
    Answered,   // a device gave its account's card, and it checked out
    NotReached, // no device answered (offline, or what it gave did not hold)
  };
  enum class Directory {
    NotAsked,    // no Account ID to look up
    Answered,    // our directory knows the account
    NotFound,    // our directory does not know it
    Unreachable, // our directory could not be asked (offline, a 5xx, none configured)
  };
  Devices devices = Devices::NotAsked;
  Directory directory = Directory::NotAsked;
  bool name_changed = false;
  bool photo_changed = false;
  bool keys_changed = false;
  bool devices_changed = false;
  /** Something else stored changed (a route, an address). */
  bool other_changed = false;
  /**
   * The directory's answer carried keys that do not verify against the contact's Account ID, or answered for
   * another account: its keys were not used (and an answer for another account not at all).
   */
  bool directory_keys_rejected = false;
  /** The contact as stored after the refresh; empty when it is gone. */
  std::optional<Contact> contact;

  bool Changed() const { return name_changed || photo_changed || keys_changed || devices_changed || other_changed; }
  /** Neither the devices nor the directory answered. */
  bool NothingAnswered() const { return devices != Devices::Answered && directory != Directory::Answered; }
};

/** What the sources gave, for MergeContactRefresh. */
struct ContactRefreshInputs {
  /** The account's kept card before and after the devices were asked (and the directory's keys kept). */
  std::optional<PeerCardRecord> card_before;
  std::optional<PeerCardRecord> card_after;
  /** A device answered now: its card's nickname is the account's own, fresh word. */
  bool card_fresh = false;
  /** The directory's answer, once it is known to be for this contact's account. */
  std::optional<mesh::DirectoryHit> hit;
  /** The answer's keys verified against its Account ID (and were kept): that Account ID may be adopted. */
  bool hit_keys_verified = false;
};

/**
 * Folds a refresh into `contact` and says what changed. Keys and devices are the account's verified card's
 * (the key stores hold the keys; the card's devices become device rows); the nickname is a card's fetched
 * now from a device, else the directory's; the photo and the device addresses are the directory's. Ids:
 * the Account ID is adopted only from an answer whose keys verified. The user's own fields (remark, trust) are
 * never touched.
 */
Contact MergeContactRefresh(Contact contact, const ContactRefreshInputs& inputs, int64_t now_ms,
                            ContactRefreshResult& result);

/**
 * The contact page's Refresh (multi-relay R0): asks, in order of trust, the contact's devices over the mesh
 * for its account's card (a forced ask, ContactCardExchange::AskDevices), then our directory by Account ID (the
 * only id it knows people by, multi-relay MR001). Keys are kept only when they verify against
 * the Account ID (the key stores refuse any other: AccountKeyResolver, VerifyKeyCard). The directory step
 * runs on the blocking lane; the merge, the write and `done` run on the UI lane. One run per contact at a
 * time.
 */
class ContactRefreshWorkflow {
public:
  ContactRefreshWorkflow(ContactsStore& contacts, AccountKeyResolver& keys, mesh::IDirectoryClient& directory,
                         ContactCardExchange& cards);
  ~ContactRefreshWorkflow();

  using Post = std::function<void(std::function<void()>)>;
  /** Where the directory lookup runs (a worker) and where the result lands (the UI thread); tests queue them. */
  void SetPosts(Post blocking, Post ui);

  /**
   * Refreshes `contact_id`; `done` once, on the UI lane. False (and no `done`) when one already runs for it or
   * the contact is unknown.
   */
  bool Start(const std::string& contact_id, std::function<void(ContactRefreshResult)> done);
  bool Running(const std::string& contact_id) const;

private:
  struct Run;
  void AskDirectory(const std::shared_ptr<Run>& run);
  void TakeDirectoryAnswer(Run& run, mesh::DirectoryHit hit);
  void Finish(const std::shared_ptr<Run>& run);

  ContactsStore& contacts_;
  AccountKeyResolver& keys_;
  mesh::IDirectoryClient& directory_;
  ContactCardExchange& cards_;
  Post post_blocking_;
  Post post_ui_;
  mutable std::mutex mutex_;
  std::set<std::string> running_;
  /** Outlives this in the posted steps' captures. */
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace pbr
