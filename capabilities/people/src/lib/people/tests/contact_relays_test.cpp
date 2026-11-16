// A contact's relays (multi-relay MR002, R4): its account's newest card's once one is kept, else the origin the QR
// locator gave, else none known.
#include "lib/people/ContactIdentity.h"
#include "lib/people/ContactJson.h"

#include "lib/people/tests/TestAccount.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

using Origins = std::vector<std::string>;

Contact AddedByQr(const std::string& locator_relay) {
  Contact contact;
  contact.id = "c1";
  contact.remote.ids = {{mesh::ContactIdKind::Account, "account:bob", true}};
  contact.remote.locator_relay = locator_relay;
  contact.remote.locator_relay_peer_id = locator_relay.empty() ? "" : "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP";
  SyncContactMirrors(contact);
  return contact;
}

PeerCardRecord WithCard(std::vector<std::string> origins) {
  PeerCardRecord record;
  record.keys.account_id = "account:bob";
  AccountCard card;
  card.keys = record.keys;
  uint8_t fill = 0x40;
  for (std::string& origin : origins) {
    card.relays.push_back(test::CardRelay(origin, fill++));
  }
  record.newest_card = card;
  return record;
}

TEST(ContactRelaysTest, AContactAddedByQrStartsWithTheLocatorsOrigin) {
  const ContactRelayList list = ContactRelays(AddedByQr("https://relay.example/api/relay"), std::nullopt);
  EXPECT_EQ(list.origins, Origins({"https://relay.example/api/relay"}));
  EXPECT_FALSE(list.from_card);
}

TEST(ContactRelaysTest, OnlyKeysKnownStillMeansTheLocatorsOrigin) {
  PeerCardRecord keys_only;
  keys_only.keys.account_id = "account:bob"; // a directory's keys: no account card, so no list
  EXPECT_EQ(ContactRelays(AddedByQr("https://relay.example"), keys_only).origins, Origins({"https://relay.example"}));
}

TEST(ContactRelaysTest, OnceACardIsKeptItsListIsTheContacts) {
  const ContactRelayList list =
      ContactRelays(AddedByQr("https://relay.example"), WithCard({"https://b.example", "https://c.example"}));
  EXPECT_EQ(list.origins, Origins({"https://b.example", "https://c.example"}));
  EXPECT_TRUE(list.from_card);
}

TEST(ContactRelaysTest, ACardListingNoRelayMeansTheMeshOnly) {
  const ContactRelayList list = ContactRelays(AddedByQr("https://relay.example"), WithCard({}));
  EXPECT_TRUE(list.origins.empty());
  EXPECT_TRUE(list.from_card);
}

TEST(ContactRelaysTest, NoCardAndNoLocatorMeansNoneKnown) {
  const ContactRelayList list = ContactRelays(AddedByQr(""), std::nullopt);
  EXPECT_TRUE(list.origins.empty());
  EXPECT_FALSE(list.from_card);
}

// MR008: the locator's relay is kept as its address, the origin with the PeerId it answers to.
TEST(ContactRelaysTest, TheLocatorsRelayAddressIsStoredWithTheContact) {
  const Contact again = ContactFromJson(ContactToJson(AddedByQr("https://relay.example/api/relay")));
  EXPECT_EQ(again.remote.locator_relay, "https://relay.example/api/relay");
  EXPECT_EQ(again.remote.locator_relay_peer_id, "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP");
  EXPECT_EQ(ContactRelays(again, std::nullopt).relays.at(0).peer_id, again.remote.locator_relay_peer_id);
  EXPECT_FALSE(ContactToJson(AddedByQr("")).getObject("remote")->contains("locator_relay")) << "left out when none";

  Object origin_only = ContactToJson(AddedByQr(""));
  Object remote = *origin_only.getObject("remote");
  remote.set("locator_relay", std::string("https://relay.example/api/relay")); // R4 before MR008: no PeerId
  origin_only.set("remote", ObjectValue(std::move(remote)));
  EXPECT_TRUE(ContactFromJson(origin_only).remote.locator_relay.empty()) << "an origin alone is no address";
}

} // namespace
} // namespace pbr
