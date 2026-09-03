#include "domain/ui/CallConflictCopy.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

namespace {

void LoadLocales() {
#ifdef PP_BROWSER_ASSETS_DIR
  ASSERT_TRUE(pbr::LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
#endif
  pbr::LocalizationService::Instance().SetPreferredLanguage("en");
}

} // namespace

TEST(CallConflictCopyTest, NoConflictUsesAcceptDecline) {
  LoadLocales();
  const pbr::CallConflictCopy copy = pbr::MakeCallConflictCopy(false, false, "Alice", "Bob");
  EXPECT_EQ(copy.eyebrow, "Incoming call");
  EXPECT_EQ(copy.accept_label, "Accept");
  EXPECT_EQ(copy.decline_label, "Decline");
  EXPECT_TRUE(copy.hint.empty());
}

TEST(CallConflictCopyTest, SamePeerCallbackHint) {
  LoadLocales();
  const pbr::CallConflictCopy copy = pbr::MakeCallConflictCopy(true, true, "Alice", "Alice");
  EXPECT_EQ(copy.eyebrow, "You're already calling");
  EXPECT_EQ(copy.accept_label, "End & Accept");
  EXPECT_EQ(copy.decline_label, "Ignore");
  EXPECT_EQ(copy.hint, "Alice is calling you back. Answering ends your outgoing call.");
}

TEST(CallConflictCopyTest, DifferentPeerEndsCurrentCall) {
  LoadLocales();
  const pbr::CallConflictCopy copy = pbr::MakeCallConflictCopy(true, false, "Carol", "Bob");
  EXPECT_EQ(copy.accept_label, "End & Accept");
  EXPECT_EQ(copy.decline_label, "Ignore");
  EXPECT_EQ(copy.hint, "Answering will end your current call with Bob.");
}
