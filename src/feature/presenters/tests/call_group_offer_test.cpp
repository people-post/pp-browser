#include "feature/presenters/CallPresenter.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

// G022: "Make a group…" and the group banner act only on what the call screen shows; a stale tap does nothing.

TEST(CallGroupOfferTest, WithNoCallOnScreenTheButtonsDoNothing) {
  CallPresenter presenter;
  int opened = 0;
  CallPresenter::Callbacks callbacks;
  callbacks.open_make_group = [&](const std::string&) { ++opened; };
  presenter.BindCallbacks(std::move(callbacks));
  CallFunctionalPorts ports;
  int joins = 0;
  ports.join_group_invite = [&](const std::string&, std::function<void(Roe<void>)>) { ++joins; };
  presenter.BindPorts(std::move(ports));

  presenter.OpenMakeGroup();
  EXPECT_EQ(opened, 0) << "no call: no picker";
  bool answered = false;
  presenter.JoinGroupInvite([&](Roe<void> joined) {
    answered = true;
    EXPECT_FALSE(joined) << "no banner: nothing to join";
  });
  EXPECT_TRUE(answered);
  EXPECT_EQ(joins, 0) << "no invite was accepted";
  presenter.PutOffGroupInvite(); // nothing to put off; no crash
  EXPECT_FALSE(presenter.Screen().group_invite);
  EXPECT_FALSE(presenter.Screen().show_make_group);
}

TEST(CallGroupOfferTest, TheWordsExistInEnglishAndChinese) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  for (const char* key : {"call.make_group", "call.group_invite.banner", "call.group_invite.join",
                          "call.group_invite.not_now", "chat.group_event.made_in_call", "chat.group_invite.body_call",
                          "people_picker.title_call_make_group", "people_picker.in_this_call"}) {
    LocalizationService::Instance().SetPreferredLanguage("en");
    const std::string en = Tr(key);
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
    const std::string zh = Tr(key);
    EXPECT_FALSE(en.empty()) << key;
    EXPECT_NE(en, key);
    EXPECT_NE(zh, en) << key << ": no Chinese words (English fallback)";
  }
  LocalizationService::Instance().SetPreferredLanguage("en");
  // A label that leads to a confirming step ends with "…" (UI_DESIGN_SYSTEM copy rules).
  const std::string label = Tr("call.make_group");
  EXPECT_EQ(label.substr(label.size() - std::string("…").size()), "…");
  EXPECT_EQ(Tr("call.group_invite.banner", {{"name", "Ann"}, {"group", "Ann, Bo, Cy"}}),
            "Ann made a group “Ann, Bo, Cy” with everyone here");
}

} // namespace
} // namespace pbr
