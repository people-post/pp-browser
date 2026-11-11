#include "feature/presenters/ChatPresenter.h"

#include <gtest/gtest.h>

using namespace pbr;

namespace {

PeerDisplayLabel Named(const std::string& title, const PeerLabelTrust trust) {
  PeerDisplayLabel label;
  label.title = title;
  label.trust = trust;
  return label;
}

// A resolver the way the app has it: "you" for this device, a contact, a directory nickname, and a raw id for
// anyone else (the short id the resolver would show).
PeerDisplayLabel Resolve(const std::string& sender) {
  if (sender == kLocalSelfContactId) {
    return Named("You", PeerLabelTrust::Contact);
  }
  if (sender == "account:amy") {
    return Named("Amy", PeerLabelTrust::Contact);
  }
  if (sender == "account:nick") {
    return Named("~nick", PeerLabelTrust::DirectoryUnverified);
  }
  return Named("relay:lHaEnkO4\xE2\x80\xA6", PeerLabelTrust::RawId);
}

std::string NoIcon(const std::string&) {
  return {};
}

TranscriptReaction Chip(const std::string& emoji, std::vector<std::string> senders) {
  TranscriptReaction chip;
  chip.emoji = emoji;
  chip.count = senders.size();
  chip.senders = std::move(senders);
  return chip;
}

} // namespace

TEST(ReactorRowsTest, OwnReactionComesFirstInItsGroupAndGroupsKeepTheirOrder) {
  const auto rows =
      BuildReactorRows({Chip("A", {"account:amy", kLocalSelfContactId, "account:nick"}), Chip("B", {"account:amy"})},
                       Resolve, NoIcon, "Group member");
  ASSERT_EQ(rows.size(), 4u);
  EXPECT_EQ(rows[0].emoji, "A");
  EXPECT_TRUE(rows[0].is_self);
  EXPECT_EQ(rows[0].title, "You");
  EXPECT_EQ(rows[1].identity, "account:amy");
  EXPECT_EQ(rows[2].identity, "account:nick");
  EXPECT_EQ(rows[3].emoji, "B");
  EXPECT_FALSE(rows[3].is_self);
}

TEST(ReactorRowsTest, ANonContactWithADirectoryNicknameShowsTheNickname) {
  const auto rows = BuildReactorRows({Chip("A", {"account:nick"})}, Resolve, NoIcon, "Group member");
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].title, "~nick");
}

TEST(ReactorRowsTest, AnUnknownSenderShowsTheFallbackNameNeverTheId) {
  const auto rows = BuildReactorRows({Chip("A", {"account:gone"})}, Resolve, NoIcon, "Group member");
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].title, "Group member");
  EXPECT_EQ(rows[0].title.find("relay:"), std::string::npos);
  EXPECT_EQ(rows[0].stable_id, "account:gone") << "the avatar colour still follows the person";
}

TEST(ReactorRowsTest, NoReactionsNoRows) {
  EXPECT_TRUE(BuildReactorRows({}, Resolve, NoIcon, "Group member").empty());
  ChatPresenter without_chats; // no conversations bound: nothing to look up
  EXPECT_TRUE(without_chats.ReactorsOf("m1").empty()) << "a message that is gone lists nobody";
}
