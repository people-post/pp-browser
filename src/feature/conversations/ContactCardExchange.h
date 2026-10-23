#pragma once

#include "common/chat/ChatPayloadTypes.h"
#include "domain/peers/AccountKeyResolver.h"
#include "lib/people/ContactsStore.h"

#include "common/Error.h"
#include "common/Module.h"

#include <cstdint>
#include <string>
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

private:
  ContactsStore& contacts_;
  AccountKeyResolver& keys_;
};

} // namespace pbr
