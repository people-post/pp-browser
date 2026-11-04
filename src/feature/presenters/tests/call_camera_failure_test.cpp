#include "feature/presenters/CallPresenter.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

// A camera that did not come on is told in plain words picked by its kind; the engine's text is for the log.

std::vector<CallNotice> Report(const CameraFailure& failure) {
  CallPresenter presenter;
  std::vector<CallNotice> notices;
  CallPresenter::Callbacks callbacks;
  callbacks.notify = [&](const CallNotice& notice) { notices.push_back(notice); };
  presenter.BindCallbacks(std::move(callbacks));
  presenter.ReportCameraFailure(failure);
  return notices;
}

TEST(CallCameraFailureTest, EachKindIsToldWithItsOwnWordsAndNeverTheEngineText) {
  const struct {
    CameraFailure::Kind kind;
    const char* key;
  } cases[] = {
      {CameraFailure::Kind::Busy, "call.error.camera_busy"},
      {CameraFailure::Kind::NotResponding, "call.error.camera_not_responding"},
      {CameraFailure::Kind::Failed, "call.error.camera_failed"},
  };
  for (const auto& c : cases) {
    const auto notices = Report({.kind = c.kind, .detail = "SDL_OpenCamera failed: Cannot lockForConfiguration"});
    ASSERT_EQ(notices.size(), 1u) << c.key;
    EXPECT_EQ(notices[0].kind, CallNotice::Kind::Error);
    EXPECT_EQ(notices[0].key, c.key);
    EXPECT_TRUE(notices[0].text.empty()) << "raw text stays in the log";
  }
}

TEST(CallCameraFailureTest, TheWordsExistInEnglishAndChinese) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  for (const char* key : {"call.error.camera_busy", "call.error.camera_not_responding", "call.error.camera_failed"}) {
    LocalizationService::Instance().SetPreferredLanguage("en");
    const std::string en = Tr(key);
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
    const std::string zh = Tr(key);
    EXPECT_FALSE(en.empty()) << key;
    EXPECT_NE(en, key);
    EXPECT_NE(zh, en) << key << ": no Chinese words (English fallback)";
  }
  LocalizationService::Instance().SetPreferredLanguage("en");
}

} // namespace
} // namespace pbr
