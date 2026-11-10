// The ring over a call (CALLS.md "Conflict", V064): it says who is calling and which call answering ends.
#include "lib/ui/CallConflictCopy.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

namespace {

void LoadLocales(const char* language = "en") {
#ifdef PP_BROWSER_ASSETS_DIR
  ASSERT_TRUE(pbr::LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
#endif
  pbr::LocalizationService::Instance().SetPreferredLanguage(language);
}

pbr::CallConflictNames Names(std::string caller, std::string ending, bool group = false, bool same_peer = false) {
  return {.caller = std::move(caller), .ending = std::move(ending), .ending_group = group, .same_peer = same_peer};
}

} // namespace

TEST(CallConflictCopyTest, NoConflictUsesAcceptDecline) {
  LoadLocales();
  const pbr::CallConflictCopy copy = pbr::MakeCallConflictCopy(false, Names("Alice", "Bob"));
  EXPECT_EQ(copy.eyebrow, "Incoming call");
  EXPECT_TRUE(copy.title.empty()) << "the ring says what kind of call it is";
  EXPECT_EQ(copy.accept_label, "Accept");
  EXPECT_EQ(copy.decline_label, "Decline");
  EXPECT_TRUE(copy.hint.empty());
}

TEST(CallConflictCopyTest, SaysWhoIsCallingAndWhoTheCallThatEndsIsWith) {
  LoadLocales();
  const pbr::CallConflictCopy copy = pbr::MakeCallConflictCopy(true, Names("Carol", "Bob"));
  EXPECT_EQ(copy.eyebrow, "You're already in a call");
  EXPECT_EQ(copy.title, "Carol is calling");
  EXPECT_EQ(copy.accept_label, "End & accept");
  EXPECT_EQ(copy.decline_label, "Ignore");
  EXPECT_EQ(copy.hint, "Accepting ends your call with Bob.");
}

TEST(CallConflictCopyTest, AGroupsCallIsNamedByItsGroup) {
  LoadLocales();
  EXPECT_EQ(pbr::MakeCallConflictCopy(true, Names("Carol", "Family", true)).hint,
            "Accepting ends your call in Family.");
  EXPECT_EQ(pbr::MakeCallConflictCopy(true, Names("Carol", "", true)).hint, "Accepting ends your group call.");
}

TEST(CallConflictCopyTest, UnknownNamesStillSayTheCallEnds) {
  LoadLocales();
  const pbr::CallConflictCopy copy = pbr::MakeCallConflictCopy(true, Names("", ""));
  EXPECT_EQ(copy.title, "Someone is calling");
  EXPECT_EQ(copy.hint, "Accepting ends your current call.");
}

TEST(CallConflictCopyTest, TheSamePeerCallingAgainEndsTheOtherCallWithThem) {
  LoadLocales();
  const pbr::CallConflictCopy copy = pbr::MakeCallConflictCopy(true, Names("Alice", "Alice", false, true));
  EXPECT_EQ(copy.title, "Alice is calling");
  EXPECT_EQ(copy.hint, "Accepting ends your other call with Alice.");
}

TEST(CallConflictCopyTest, ChineseNamesTheCallerAndTheCallThatEnds) {
  LoadLocales("zh-Hans");
  const pbr::CallConflictCopy copy = pbr::MakeCallConflictCopy(true, Names("Carol", "Bob"));
  EXPECT_EQ(copy.title, "Carol 来电");
  EXPECT_EQ(copy.hint, "接听将结束你与 Bob 的通话。");
  EXPECT_EQ(copy.accept_label, "结束并接听");
  LoadLocales("en");
}

// Call pages never name someone by their account id: no known name reads "Stranger" / "陌生人" and flags the silhouette.
TEST(CallerNameTest, ANameIsKeptAndNoNameIsAStrangerInBothLanguages) {
  LoadLocales("en");
  const pbr::CallerName known = pbr::MakeCallerName("Alice");
  EXPECT_EQ(known.label, "Alice");
  EXPECT_FALSE(known.unknown);
  const pbr::CallerName stranger = pbr::MakeCallerName("");
  EXPECT_EQ(stranger.label, "Stranger");
  EXPECT_TRUE(stranger.unknown);
  LoadLocales("zh-Hans");
  EXPECT_EQ(pbr::MakeCallerName("").label, "陌生人");
  EXPECT_EQ(pbr::MakeCallerName("爱丽丝").label, "爱丽丝");
  LoadLocales("en");
}

// The incoming-call notification for a caller with no name (CallController picks the key: identity but no name).
TEST(CallerNameTest, TheStrangerNotificationReadsNaturallyInBothLanguages) {
  LoadLocales("en");
  EXPECT_EQ(pbr::Tr("call.notify.body_stranger"), "Stranger is calling you");
  LoadLocales("zh-Hans");
  EXPECT_EQ(pbr::Tr("call.notify.body_stranger"), "陌生人正在呼叫你");
  EXPECT_NE(pbr::Tr("call.notify.body_stranger"), pbr::Tr("call.notify.body_unknown"));
  LoadLocales("en");
}

// A stranger starting a group call: the ring's subtitle names the Stranger, not the id.
TEST(CallerNameTest, TheGroupRingSubtitleNamesAStranger) {
  LoadLocales("en");
  EXPECT_EQ(pbr::Tr("call.ring.phone.group_voice", {{"name", pbr::MakeCallerName("").label}}),
            "Stranger started a voice call");
  LoadLocales("zh-Hans");
  EXPECT_EQ(pbr::Tr("call.ring.phone.group_video", {{"name", pbr::MakeCallerName("").label}}), "陌生人 发起了视频通话");
  LoadLocales("en");
}

// One word for a peer in every sentence's {name} slot: a stranger is "Stranger", never blank; no identity: blank.
TEST(CallConflictCopyTest, APeerWithoutANameIsAStrangerInEverySentenceAndNoIdentityIsBlank) {
  LoadLocales();
  EXPECT_EQ(pbr::PeerLabel("acct-1", "Bob"), "Bob");
  EXPECT_EQ(pbr::PeerLabel("acct-1", ""), "Stranger");
  EXPECT_EQ(pbr::PeerLabel("", ""), "");
  EXPECT_EQ(pbr::PeerLabel("", "Bob"), "");

  const std::string stranger = pbr::PeerLabel("acct-1", "");
  const pbr::CallConflictCopy copy = pbr::MakeCallConflictCopy(true, Names(stranger, stranger, false, true));
  EXPECT_EQ(copy.title, "Stranger is calling");
  EXPECT_EQ(copy.hint, "Accepting ends your other call with Stranger.") << "same peer wording keeps the caller's word";
  const pbr::CallConflictCopy other = pbr::MakeCallConflictCopy(true, Names("Carol", stranger));
  EXPECT_EQ(other.hint, "Accepting ends your call with Stranger.");
}
