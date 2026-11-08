#include "feature/presenters/CallPresenter.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

// V062: a guest's buttons act only on the call on screen; a stale tap does nothing and says so.
TEST(CallGuestActionsTest, WithNoCallOnScreenTheGuestButtonsDoNothing) {
  CallPresenter presenter;
  CallFunctionalPorts ports;
  int invites = 0;
  ports.invite_guest_to_group = [&](const std::string&, const std::string&, const std::string&,
                                    std::function<void(Roe<void>)>) { ++invites; };
  presenter.BindPorts(std::move(ports));

  bool removed_answered = false;
  presenter.RemoveFromCall("account:erin", [&](Roe<void> removed) {
    removed_answered = true;
    EXPECT_FALSE(removed);
  });
  EXPECT_TRUE(removed_answered);
  bool invited_answered = false;
  presenter.AddGuestToGroup("account:erin", [&](Roe<void> invited) {
    invited_answered = true;
    EXPECT_FALSE(invited);
  });
  EXPECT_TRUE(invited_answered);
  EXPECT_EQ(invites, 0) << "no group call: no group invite";
  EXPECT_TRUE(presenter.MayAddToActiveCall("account:erin")) << "no group call: the picker is unchanged";
}

// V062: the guest words in English and Chinese; "Remove from call…" asks first, so it ends with "…".
TEST(CallGuestActionsTest, TheWordsExistInEnglishAndChinese) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  for (const char* key :
       {"call.guest.label", "call.guest.joined", "call.guest.joined_yours", "call.guest.line", "call.guest.line_yours",
        "call.guest.add_to_group", "call.guest.remove", "call.guest.remove_title", "call.guest.remove_message",
        "call.guest.remove_ok", "call.guest.invited_to_group", "call.ring.guest_notice"}) {
    LocalizationService::Instance().SetPreferredLanguage("en");
    const std::string en = Tr(key);
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
    const std::string zh = Tr(key);
    EXPECT_FALSE(en.empty()) << key;
    EXPECT_NE(en, key);
    EXPECT_NE(zh, en) << key << ": no Chinese words (English fallback)";
  }
  LocalizationService::Instance().SetPreferredLanguage("en");
  const std::string remove = Tr("call.guest.remove");
  EXPECT_EQ(remove.substr(remove.size() - std::string("…").size()), "…");
  EXPECT_EQ(Tr("call.guest.joined", {{"name", "Erin"}, {"bringer", "Bob"}}), "Erin joined as Bob's guest");
  EXPECT_EQ(Tr("call.ring.guest_notice", {{"count", "3"}}), "Joining shows you to the 3 people in this call.");
  EXPECT_EQ(Tr("call.guest.add_to_group", {{"name", "Erin"}}), "Add Erin to the group");
}

} // namespace
} // namespace pbr
