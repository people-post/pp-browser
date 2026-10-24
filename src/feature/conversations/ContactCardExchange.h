#pragma once

#include "common/chat/ChatPayloadTypes.h"
#include "lib/messaging/ContactLocator.h"
#include "domain/peers/AccountKeyResolver.h"
#include "lib/people/ContactsStore.h"

#include "common/Error.h"
#include "common/Module.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Contacts passed hand to hand (docs/contracts/ACCOUNT_CARD.md, contact_card sub_version 2): the card a
 * contact is shared with — its account's newest signed card, else its key card — and what a received
 * one adds once it checks out: the account's keys and a contact. No directory involved.
 */
class ContactCardExchange : public Module {
public:
  ContactCardExchange(ContactsStore& contacts, AccountKeyResolver& keys);

  /** The contact card for `contact_id` (its name, its account's card, its relay route when known). */
  Roe<ChatContactCardFields> CardFor(const std::string& contact_id);
  /**
   * Checks a received card and keeps it: the account's keys and devices, and a contact — added, or the
   * existing one for that account. The relay hint is the sharer's, unsigned: it is taken for a new contact
   * only, never to reroute one already known.
   */
  Roe<Contact> Add(const ChatContactCardFields& card, int64_t now_ms);

  /** Asks device `peer_id` for `account_id`'s card over Amp (AccountCardService::Fetch); `done` once, any thread. */
  using CardFetcher = std::function<void(const std::string& peer_id, const std::string& account_id,
                                         std::function<void(Roe<PeerCardRecord>)> done)>;
  void SetCardFetcher(CardFetcher fetcher) { fetch_card_ = std::move(fetcher); }
  /** Where a blocking step (a directory lookup) runs; the background worker unless a test says otherwise. */
  void SetBlockingPost(std::function<void(std::function<void()>)> post) { post_blocking_ = std::move(post); }

  /** What adding from a locator came to: the contact, and whether its keys are known yet. */
  struct LocatorAdded {
    Contact contact;
    bool keys_known = false;
  };
  /**
   * Adds the person a locator names (QR code, invite line): the contact (its listed devices as endpoint
   * rows), then the account's card — from a listed device over Amp, else the directory when one is
   * configured. Without keys yet the contact still stands; the person's first message brings them. `done`
   * once, on a worker or I/O thread.
   */
  void AddFromLocator(const ContactLocator& locator, std::function<void(Roe<LocatorAdded>)> done);

private:
  void FetchFromDevices(std::vector<std::string> devices, const std::string& account_id,
                        std::function<void(bool kept)> done);
  CardFetcher fetch_card_;
  std::function<void(std::function<void()>)> post_blocking_;
  ContactsStore& contacts_;
  AccountKeyResolver& keys_;
};

} // namespace pbr
