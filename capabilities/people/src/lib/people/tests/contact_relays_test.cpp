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
  contact.remote.locator_relay_id =
      locator_relay.empty() ? RelayId{} : RelayId::FromRaw(ByteVector(kRelayIdBytes, 0x07)).value();
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

// MR008 (SIGNED_FORMATS § 3.3): the locator's relay is kept as its address, the origin with its relay id.
TEST(ContactRelaysTest, TheLocatorsRelayAddressIsStoredWithTheContact) {
  const Contact again = ContactFromJson(ContactToJson(AddedByQr("https://relay.example/api/relay")));
  EXPECT_EQ(again.remote.locator_relay, "https://relay.example/api/relay");
  EXPECT_EQ(again.remote.locator_relay_id, RelayId::FromRaw(ByteVector(kRelayIdBytes, 0x07)).value());
  EXPECT_EQ(ContactRelays(again, std::nullopt).relays.at(0).relay_id, again.remote.locator_relay_id);
  EXPECT_NE(ContactToJson(again).getObject("remote")->getObject("locator_relay")->getString("relay_id"), std::nullopt);
  EXPECT_FALSE(ContactToJson(AddedByQr("")).getObject("remote")->contains("locator_relay")) << "left out when none";

  Object origin_only = ContactToJson(AddedByQr(""));
  Object remote = *origin_only.getObject("remote");
  remote.set("locator_relay", std::string("https://relay.example/api/relay")); // R4 before MR008: no relay id
  origin_only.set("remote", ObjectValue(std::move(remote)));
  EXPECT_TRUE(ContactFromJson(origin_only).remote.locator_relay.empty()) << "an origin alone is no address";
  Object with_peer_id = ContactToJson(AddedByQr(""));
  Object peer_remote = *with_peer_id.getObject("remote");
  Object named;
  named.set("origin", std::string("https://relay.example/api/relay"));
  named.set("peer_id", std::string("QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP")); // the shape before the cutover
  peer_remote.set("locator_relay", ObjectValue(std::move(named)));
  with_peer_id.set("remote", ObjectValue(std::move(peer_remote)));
  EXPECT_TRUE(ContactFromJson(with_peer_id).remote.locator_relay.empty()) << "a PeerId is no relay id: no address";
}

} // namespace
} // namespace pbr
