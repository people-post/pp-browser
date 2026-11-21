// Me → My relays (multi-relay R3, MR006): the list preferred first, each relay with its registration and
// its last poll; on the main device every relay but the preferred one may be made preferred or removed, and another
// added while there are fewer than four.
#include "feature/settings/MyRelays.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

namespace {

using pbr::MyRelayFacts;
using pbr::RelayRegistrationRecord;

void LoadLocales() {
#ifdef PP_BROWSER_ASSETS_DIR
  ASSERT_TRUE(pbr::LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
#endif
  pbr::LocalizationService::Instance().SetPreferredLanguage("en");
}

MyRelayFacts Relay(const std::string& host, RelayRegistrationRecord registration = {},
                   MyRelayFacts::Reach reach = MyRelayFacts::Reach::Unknown) {
  return MyRelayFacts{
      .origin = "https://" + host + "/api/relay", .registration = std::move(registration), .reach = reach};
}

RelayRegistrationRecord Registered(const std::string& until) {
  return {.state = RelayRegistrationRecord::State::Registered, .expires_at = until};
}

RelayRegistrationRecord Failed(const std::string& reason) {
  return {.state = RelayRegistrationRecord::State::Failed, .reason = reason};
}

} // namespace

TEST(MyRelaysTest, EachRelaySaysItsRegistrationAndItsLastPoll) {
  LoadLocales();
  const pbr::MyRelaysView view = pbr::MyRelaysViewFor(
      {Relay("a.example", Registered("2026-12-01T00:00:00.000Z"), MyRelayFacts::Reach::Ok), Relay("b.example:8443"),
       Relay("c.example", Failed("unreachable"), MyRelayFacts::Reach::Failed)},
      true);
  ASSERT_EQ(view.relays.size(), 3u);
  EXPECT_EQ(view.relays[0].label, "a.example");
  EXPECT_EQ(view.relays[0].origin, "https://a.example/api/relay");
  EXPECT_TRUE(view.relays[0].preferred);
  EXPECT_EQ(view.relays[0].state, "Registered until 2026-12-01");
  EXPECT_EQ(view.relays[0].state_kind, "ok");
  EXPECT_EQ(view.relays[0].reach, "Reachable");

  EXPECT_EQ(view.relays[1].label, "b.example:8443");
  EXPECT_EQ(view.relays[1].state, "Registering…");
  EXPECT_EQ(view.relays[1].state_kind, "pending");
  EXPECT_EQ(view.relays[1].reach, "") << "not polled yet";

  EXPECT_EQ(view.relays[2].state, "Couldn't register: the relay didn't answer") << "in words, not the error's";
  EXPECT_EQ(view.relays[2].state_kind, "error");
  EXPECT_TRUE(view.relays[2].reach_failed);
}

// D3 / D4: a failure says why in the person's words (its code, never the error's English); a preferred relay the
// identity holds no registration on says so (and where to set it up), the others wait for it; an ended one says when.
TEST(MyRelaysTest, ARegistrationSaysWhyItFailedWhetherItEndedAndWhenOneIsMissing) {
  LoadLocales();
  const pbr::MyRelaysView view = pbr::MyRelaysViewFor(
      {Relay("a.example", {.state = RelayRegistrationRecord::State::NotRegistered}),
       Relay("b.example", {.state = RelayRegistrationRecord::State::NotRegistered}),
       Relay("c.example", {.state = RelayRegistrationRecord::State::Expired, .expires_at = "2026-01-02T00:00:00Z"}),
       Relay("d.example", Failed("refused")), Relay("e.example", Failed("upgrade"))},
      true);
  EXPECT_EQ(view.relays[0].state, "Not registered. Set up your account in My profile to get mail here.");
  EXPECT_EQ(view.relays[0].state_kind, "error");
  EXPECT_EQ(view.relays[1].state, "Registers after your preferred relay");
  EXPECT_EQ(view.relays[1].state_kind, "pending");
  EXPECT_EQ(view.relays[2].state, "Registration ended 2026-01-02");
  EXPECT_EQ(view.relays[2].state_kind, "error");
  EXPECT_EQ(view.relays[3].state, "Couldn't register: the relay said no");
  EXPECT_EQ(view.relays[4].state, "Needs a newer version of the app") << "426 client_upgrade_required there";
  EXPECT_EQ(view.relays[4].state_kind, "error");
  // U11: a relay older than this app says the relay needs an update, not the app; X12: a card it did not take.
  const pbr::MyRelaysView older =
      pbr::MyRelaysViewFor({Relay("a.example", Failed("relay_upgrade")), Relay("b.example", Failed("card"))}, true);
  EXPECT_EQ(older.relays[0].state, "The relay needs an update") << "400 relay_upgrade_required there";
  EXPECT_EQ(older.relays[0].state_kind, "error");
  EXPECT_EQ(older.relays[1].state, "Couldn't register: the relay didn't accept this account's card");
  EXPECT_EQ(pbr::MyRelaysViewFor({Relay("a.example", Failed("Connection refused (curl 7)"))}, true).relays[0].state,
            "Couldn't register")
      << "a reason that is not a code is not shown";
}

TEST(MyRelaysTest, OnTheMainDeviceThePreferredRelayStaysAndTheOthersMayChange) {
  LoadLocales();
  const pbr::MyRelaysView view = pbr::MyRelaysViewFor({Relay("a.example"), Relay("b.example")}, true);
  EXPECT_TRUE(view.editable);
  EXPECT_TRUE(view.can_add);
  EXPECT_FALSE(view.relays[0].can_remove) << "make another preferred first";
  EXPECT_FALSE(view.relays[0].can_make_preferred);
  EXPECT_TRUE(view.relays[1].can_remove);
  EXPECT_TRUE(view.relays[1].can_make_preferred);
}

TEST(MyRelaysTest, FourRelaysLeaveNoRoomAndALinkedDeviceOnlyShows) {
  LoadLocales();
  const auto four =
      std::vector<MyRelayFacts>{Relay("a.example"), Relay("b.example"), Relay("c.example"), Relay("d.example")};
  EXPECT_FALSE(pbr::MyRelaysViewFor(four, true).can_add);
  const pbr::MyRelaysView linked = pbr::MyRelaysViewFor({Relay("a.example"), Relay("b.example")}, false);
  EXPECT_FALSE(linked.editable);
  EXPECT_FALSE(linked.can_add);
  EXPECT_FALSE(linked.relays[1].can_remove);
  EXPECT_FALSE(linked.relays[1].can_make_preferred);
}

TEST(MyRelaysTest, ARefusedChangeSaysWhyInWords) {
  LoadLocales();
  EXPECT_EQ(pbr::MyRelaysFailureText(pp::Error("relay_origin_not_https")), "The address must start with https://.");
  EXPECT_EQ(pbr::MyRelaysFailureText(pp::Error("relay_unreachable")),
            "That relay didn't answer. Check the link and try again.");
  EXPECT_EQ(pbr::MyRelaysFailureText(pp::Error("relay_preferred_not_removable")),
            "Make another relay preferred before removing this one.");
  EXPECT_EQ(pbr::MyRelaysFailureText(pp::Error("relay_origin_local")),
            "That link is to this device or a private network. Paste a link to a relay's public address.");
  EXPECT_EQ(pbr::MyRelaysFailureText(pp::Error("relay_list_not_applied")),
            "The app was busy and didn't change your relays. Your list is as it was; try again.");
  EXPECT_EQ(pbr::MyRelaysFailureText(pp::Error("Messaging is not ready")), "Messaging is not ready");
}

// MR008 (what the user sees): a relay whose identity answer does not match its link says it is not used, is not made
// preferred, may be removed on the main device, and has nothing to accept; the org's origin's line shows on any device.
TEST(MyRelaysTest, ARelayThatDoesNotMatchItsLinkSaysItIsNotUsed) {
  LoadLocales();
  MyRelayFacts mismatch = Relay("b.example", Registered("2099-01-01T00:00:00Z"), MyRelayFacts::Reach::Failed);
  mismatch.relay_id = pbr::RelayId::Parse("relay:AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8").value();
  mismatch.identity_mismatch = true;
  const pbr::MyRelaysView view =
      pbr::MyRelaysViewFor({Relay("a.example"), mismatch}, true, "https://org.example/api/relay");
  ASSERT_EQ(view.relays.size(), 2u);
  const pbr::MyRelayRowView& row = view.relays[1];
  EXPECT_EQ(row.state, "Not used: this relay's ID doesn't match its link");
  EXPECT_EQ(row.state_kind, "error");
  EXPECT_TRUE(row.reach.empty()) << "it is not polled";
  EXPECT_TRUE(row.link.empty()) << "a link that does not answer is not handed on";
  EXPECT_TRUE(row.can_remove);
  EXPECT_FALSE(row.can_make_preferred);
  EXPECT_EQ(view.org_identity_mismatch, "https://org.example/api/relay");
}

// MR008: each relay of ours with a relay id has its relay link to copy, on every device; a bare origin has none.
TEST(MyRelaysTest, EachRelayGivesItsRelayLink) {
  LoadLocales();
  MyRelayFacts named = Relay("a.example");
  named.relay_id = pbr::RelayId::Parse("relay:AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8").value();
  const pbr::MyRelaysView view = pbr::MyRelaysViewFor({named, Relay("b.example")}, false);
  EXPECT_EQ(view.relays[0].link, "https://a.example/api/relay#relay:AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8");
  EXPECT_TRUE(view.relays[1].link.empty());
}

TEST(MyRelaysTest, AddsIdentityRefusalsAreWorded) {
  LoadLocales();
  EXPECT_EQ(pbr::MyRelaysFailureText(pbr::Error("relay_link_no_id")),
            "This link has no relay ID. Paste the relay's whole link, with the part after #.");
  EXPECT_EQ(pbr::MyRelaysFailureText(pbr::Error("relay_origin_bad_relay_id")),
            "This link's relay ID isn't right. Paste the relay's whole link again.");
  EXPECT_EQ(pbr::MyRelaysFailureText(pbr::Error("relay_identity_mismatch")),
            "This relay's ID doesn't match its link. It wasn't added.");
  EXPECT_EQ(pbr::MyRelaysFailureText(pbr::Error("relay_identity_unavailable")),
            "This server doesn't say which relay it is.");
  EXPECT_EQ(pbr::MyRelaysFailureText(pbr::Error("relay_identity_taken|b.example")),
            "This link names a relay already on your list as b.example.");
  EXPECT_EQ(pbr::MyRelaysFailureText(pbr::Error("relay_identity_taken_known|c.example")),
            "This link names a relay you already use at c.example.");
}
