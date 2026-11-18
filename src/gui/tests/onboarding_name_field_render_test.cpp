// The account-setup Name step over the real onboarding.rml and the controller's real data model: typing in the name
// field, with no Enter, turns the Register button on after the first character, and tapping it registers the typed name.
#include "common/runtime/AppRuntime.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/onboarding/OnboardingController.h"
#include "gui/shell/DataModelHost.h"

#include <ui/base/SystemInterface.h>
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

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

class QuietSystem : public ui::SystemInterface {
public:
  double GetElapsedTime() override { return 0.0; }
};

std::string View(const std::string& name) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views" / name);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return std::regex_replace(buffer.str(), std::regex(R"(\{\{i18n:[^}]*\}\})"), "");
}

constexpr const char* kContext = "onboarding-name-field-render-test";

class OnboardingNameFieldRenderTest : public ::testing::Test {
protected:
  void SetUp() override {
    OnboardingController::InstallInstance(controller_);
    AppRuntime::Initialize(AppRuntimeConfig{});
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    ui::SetRenderInterface(&render_);
    ui::SetSystemInterface(&system_);
    ASSERT_TRUE(ui::Initialise());
    context_ = ui::CreateContext(kContext, {390, 844});
    ASSERT_NE(context_, nullptr);

    OnboardingPorts ports;
    ports.load_identity = []() {
      ProfileIdentityView view;
      view.ready = true;
      view.nickname = "user"; // a fresh identity's placeholder
      view.registered = "no";
      return view;
    };
    ports.register_identity = [this](const std::string& nickname) -> Roe<void> {
      registered_.push_back(nickname);
      return {};
    };
    ports.is_completed = []() { return false; };
    ports.set_completed = [](bool) {};
    ports.mount = [](const std::string&) {};
    controller_.BindPorts(ports);
    ASSERT_TRUE(controller_.RegisterModel(context_));
    controller_.MaybeStart();
    ASSERT_TRUE(controller_.IsActive());
    document_ = context_->LoadDocumentFromMemory("<rml><head></head><body>" + View("onboarding.rml") + "</body></rml>");
    ASSERT_NE(document_, nullptr);
    document_->Show();
    context_->Update();
  }
  void TearDown() override {
    controller_.BindPorts({});
    DataModelHost::Instance().Clear();
    ui::RemoveContext(kContext);
    ui::Shutdown();
    ui::SetSystemInterface(nullptr);
    ui::SetRenderInterface(nullptr);
    AppRuntime::Shutdown();
    OnboardingController::ClearInstance();
  }

  ui::Element* Field() { return document_->QuerySelector("input.field"); }
  ui::Element* Register() { return document_->QuerySelector(".btn-primary"); }
  bool RegisterOff() {
    context_->Update();
    return Register()->HasAttribute("disabled");
  }
  /** One keystroke: the field's change event carries the whole new text, as the text input sends it. */
  void TypeSoFar(const std::string& text) {
    ui::Dictionary parameters;
    parameters["value"] = ui::String(text.c_str());
    Field()->DispatchEvent(ui::EventId::Change, parameters);
    context_->Update();
  }
  void TapRegister() {
    context_->Update();
    Register()->DispatchEvent(ui::EventId::Click, {});
    context_->Update();
  }

  NullRender render_;
  QuietSystem system_;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  OnboardingController controller_;
  std::vector<std::string> registered_;
};

TEST_F(OnboardingNameFieldRenderTest, RegisterIsOffWhileTheFieldStartsEmpty) {
  EXPECT_TRUE(RegisterOff());
  TapRegister();
  EXPECT_TRUE(registered_.empty());
}

TEST_F(OnboardingNameFieldRenderTest, TheFirstTypedCharacterTurnsRegisterOnWithoutEnter) {
  TypeSoFar("a");
  EXPECT_FALSE(RegisterOff());
  TapRegister();
  ASSERT_EQ(registered_.size(), 1u);
  EXPECT_EQ(registered_[0], "a");
}

TEST_F(OnboardingNameFieldRenderTest, TypingAndErasingKeepsRegisterInStepWithTheField) {
  TypeSoFar("Ann");
  EXPECT_FALSE(RegisterOff());
  TypeSoFar("An");
  EXPECT_FALSE(RegisterOff());
  TypeSoFar(" ");
  EXPECT_TRUE(RegisterOff()) << "spaces alone are no name";
  TypeSoFar("");
  EXPECT_TRUE(RegisterOff());
}

TEST_F(OnboardingNameFieldRenderTest, ATapNeverClearsTheField) {
  TypeSoFar("Bo");
  TapRegister();
  ASSERT_EQ(registered_.size(), 1u);
  EXPECT_EQ(registered_[0], "Bo");
  EXPECT_FALSE(RegisterOff()) << "the tap left the typed name in place";
}

} // namespace
} // namespace pbr
