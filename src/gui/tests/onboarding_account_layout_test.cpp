// The "Your account" step of account setup (views/onboarding.rml, step "account") with the real stylesheets, in English
// and in Chinese, at the wide window's minimum width (768dp) and at a phone's (390dp): every button label on one line
// (the three pills in a row once broke "Continue" inside the word on the Mac), the buttons inside the card, 44dp tall,
// and the same for the friends step, which has the same three actions.
#include "foundation/i18n/LocalizationService.h"
#include "gui/shell/DataModelHost.h"
#include "lib/ui/DataEvent.h"

#include <gtest/gtest.h>
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

class NullRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i&, const ui::String&) override { return 0; }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

std::string Asset(const char* relative) {
  return ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative);
}

struct OnboardingModel {
  ui::String step;
  ui::String nickname = "Kenneth";
  ui::String account_id = "pp1-8f3a2c9d41b7e6050a1c";
  ui::String error;
  bool busy = false;
  bool name_blank = false;
};

class OnboardingAccountLayoutTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer_);
    initialized_ = ui::Initialise();
    ASSERT_TRUE(initialized_);
    ASSERT_TRUE(ui::LoadFontFace(Path("fonts/LatoLatin-Regular.ttf")));
    ASSERT_TRUE(ui::LoadFontFace(Path("fonts/NotoSansSC-Medium.otf"), /*fallback_face=*/true));
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  }
  void TearDown() override {
    LocalizationService::Instance().SetPreferredLanguage("en");
    DataModelHost::Instance().Clear();
    if (context_) {
      ui::RemoveContext("onboarding-account-layout-test");
    }
    if (initialized_) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }
  static std::string Path(const char* relative) { return std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative; }

  /** The step as the shell mounts it (a dialog layer on the shell's root, the wide or the phone root class). */
  void Show(const std::string& language, const int width, const bool wide, const char* step) {
    LocalizationService::Instance().SetPreferredLanguage(language);
    model_.step = step;
    context_ = ui::CreateContext("onboarding-account-layout-test", {width, 844});
    ASSERT_NE(context_, nullptr);
    ASSERT_TRUE(DataModelHost::Instance().Register(context_, "onboarding", [this](ui::DataModelConstructor& ctor) {
      ctor.Bind("step", &model_.step);
      ctor.Bind("nickname", &model_.nickname);
      ctor.Bind("account_id", &model_.account_id);
      ctor.Bind("error", &model_.error);
      ctor.Bind("busy", &model_.busy);
      ctor.Bind("name_blank", &model_.name_blank);
      for (const char* event :
           {"onboarding_name_changed", "onboarding_register", "onboarding_copy_id", "onboarding_share",
            "onboarding_next", "onboarding_find_friends", "onboarding_finish"}) {
        ctor.BindEventCallback(event, DataEvent([] {}));
      }
    }));
    const std::string view = LocalizationService::Instance().LocalizeText(Asset("views/onboarding.rml"));
    const std::string rml = "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
                            Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") +
                            "</style></head><body style=\"padding: 0; font-size: 15dp;\"><div class=\"shell-body " +
                            std::string(wide ? "shell-body--wide" : "shell-body--compact") +
                            "\" style=\"position: absolute; top: 0; left: 0; width: " + std::to_string(width) +
                            "dp; height: 844dp;\">" + view + "</div></body></rml>";
    document_ = context_->LoadDocumentFromMemory(rml);
    ASSERT_NE(document_, nullptr);
    document_->Show();
    for (int i = 0; i < 3; ++i) {
      context_->Update();
    }
  }
  void Finish() {
    document_->Close();
    context_->Update();
    DataModelHost::Instance().Clear();
    ui::RemoveContext("onboarding-account-layout-test");
    context_ = nullptr;
  }

  /** Every visible button of the shown step: one line, inside the card, 44dp or more, nothing cut off. */
  void ExpectTheButtonsFit(const size_t expected) {
    ui::Element* card = document_->QuerySelector(".shell-dialog");
    ASSERT_NE(card, nullptr);
    const ui::Vector2f c_at = card->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f c_size = card->GetBox().GetSize(ui::BoxArea::Border);
    ui::ElementList all;
    card->QuerySelectorAll(all, "button");
    std::vector<ui::Element*> buttons;
    for (ui::Element* button : all) {
      if (button->IsVisible(true)) {
        buttons.push_back(button);
      }
    }
    ASSERT_EQ(buttons.size(), expected);
    for (ui::Element* button : buttons) {
      const std::string label = button->GetInnerRML().c_str();
      SCOPED_TRACE(label);
      const ui::Vector2f at = button->GetAbsoluteOffset(ui::BoxArea::Border);
      const ui::Vector2f size = button->GetBox().GetSize(ui::BoxArea::Border);
      EXPECT_GE(size.y, 44.f) << "at least 44dp tall";
      EXPECT_LT(size.y, 60.f) << "one line of label (a wrapped label makes the pill twice as tall)";
      EXPECT_GE(at.x, c_at.x - 0.5f);
      EXPECT_LE(at.x + size.x, c_at.x + c_size.x + 0.5f) << "inside the card";
      EXPECT_LE(button->GetScrollWidth(), button->GetClientWidth() + 1.f) << "the label is not cut off or squeezed";
    }
  }

  NullRender renderer_;
  bool initialized_ = false;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  OnboardingModel model_;
};

TEST_F(OnboardingAccountLayoutTest, TheThreeAccountButtonsEachSitOnOneLineInBothLanguagesWideAndOnAPhone) {
  for (const char* language : {"en", "zh-Hans"}) {
    for (const auto& [width, wide] : {std::pair{768, true}, std::pair{390, false}, std::pair{320, false}}) {
      SCOPED_TRACE(std::string(language) + " " + std::to_string(width));
      Show(language, width, wide, "account");
      ExpectTheButtonsFit(3);
      Finish();
    }
  }
}

TEST_F(OnboardingAccountLayoutTest, TheFriendsStepsThreeButtonsFitToo) {
  for (const char* language : {"en", "zh-Hans"}) {
    for (const auto& [width, wide] : {std::pair{768, true}, std::pair{390, false}, std::pair{320, false}}) {
      SCOPED_TRACE(std::string(language) + " " + std::to_string(width));
      Show(language, width, wide, "friends");
      ExpectTheButtonsFit(3);
      Finish();
    }
  }
}

TEST_F(OnboardingAccountLayoutTest, ContinueIsTheFirstFullWidthPrimaryAboveTheTwoSecondaryActions) {
  Show("en", 768, true, "account");
  ui::ElementList all;
  document_->GetElementsByClassName(all, "btn");
  std::vector<ui::Element*> buttons;
  for (ui::Element* button : all) {
    if (button->IsVisible(true)) {
      buttons.push_back(button);
    }
  }
  ASSERT_EQ(buttons.size(), 3u);
  EXPECT_TRUE(buttons[0]->IsClassSet("btn-primary"));
  EXPECT_NE(std::string(buttons[0]->GetInnerRML().c_str()).find("Continue"), std::string::npos);
  EXPECT_LT(buttons[0]->GetAbsoluteOffset(ui::BoxArea::Border).y, buttons[1]->GetAbsoluteOffset(ui::BoxArea::Border).y);
  EXPECT_LT(buttons[1]->GetAbsoluteOffset(ui::BoxArea::Border).y, buttons[2]->GetAbsoluteOffset(ui::BoxArea::Border).y);
}

} // namespace
} // namespace pbr
