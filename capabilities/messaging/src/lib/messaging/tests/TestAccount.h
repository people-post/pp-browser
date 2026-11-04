#pragma once

// Messaging's view of a test account (lib/people/tests/TestAccount.h): signing what its device sends and
// receiving as it.

#include "lib/delivery/EnvelopeSigner.h"
#include "lib/messaging/RelayReceivePipeline.h"
#include "lib/people/tests/TestAccount.h"

#include <map>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr::test {

/**
 * Signs `envelope` as `account`'s device sends it (naming the device). A fixture that set only the read view
 * (payload, numbering, recipient device) gets it as the envelope's one copy first.
 */
inline void SignEnvelopeAs(const TestAccount& account, RelayEnvelope& envelope) {
  if (envelope.body.e2e.copies.empty()) {
    envelope.body.e2e.copies = {RelayCopy{.recipient_device = envelope.recipient_device,
                                          .session_epoch = envelope.session_epoch,
                                          .sender_seq = envelope.sender_seq,
                                          .key_init_b64 = envelope.body.e2e.key_init_b64,
                                          .payload_b64 = envelope.body.e2e.payload_b64}};
  }
  envelope.sender_device = account.device.peer_id;
  envelope.signature = account.device.Sign(EnvelopeSigner::BuildSignBytes(envelope).value()).value();
}

/** The receive pipeline's view of `account` (and its device) as the local one. */
inline LocalKeys ReceiveKeysOf(const TestAccount& account) {
  return LocalKeys{[id = account.account_id]() -> Roe<std::string> { return id; },
                   [kem = account.kem_private]() -> Roe<ByteVector> { return kem; },
                   [peer_id = account.device.peer_id]() -> Roe<std::string> { return peer_id; },
                   [kem = account.device.kem_private]() -> Roe<ByteVector> { return kem; }};
}

/** A receiver's kept cards: `accounts`' device cards (AccountCardLookup). */
inline AccountCardLookup CardsOf(std::vector<const TestAccount*> accounts) {
  std::map<std::string, AccountCard> cards;
  for (const TestAccount* account : accounts) {
    cards.emplace(account->account_id, account->DeviceCard());
  }
  return [cards = std::move(cards)](const std::string& account_id) -> std::optional<AccountCard> {
    auto found = cards.find(account_id);
    if (found == cards.end()) {
      return std::nullopt;
    }
    return found->second;
  };
}

} // namespace pbr::test
