// The Chats list's "this device is no longer on your account" notice as drawn on a phone: the real sidebar view
// (through the real ViewCatalog) over the real shell data model. It looks like the other tip cards and sits above
// them; its two buttons reach the handlers the controller binds (`device_removed_relink`, `device_removed_ok`).
#include "common/runtime/AppRuntime.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/IAssetLocator.h"
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/ChatScreenState.h"
#include "gui/shell/DataModelHost.h"
#include "gui/shell/ShellHost.h"
#include "lib/ui/DataEvent.h"
#include "lib/ui/ViewCatalog.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <regex>
#include <string>
#include <vector>

namespace pbr {
namespace {

class NullRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i& dimensions, const ui::String&) override {
    dimensions = {24, 24};
    return 2;
  }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

class AssetsLocator : public IAssetLocator {
public:
  std::string Resolve(const std::string& relative) const override {
    return (std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative).string();
  }
};

/** {{i18n:key}} filled with the current language's words, as the app's text view does. */
std::string Translated(std::string rml) {
  const std::regex key(R"(\{\{i18n:([^}]*)\}\})");
  std::smatch match;
  std::string out;
  while (std::regex_search(rml, match, key)) {
    out += match.prefix().str() + Tr(match[1].str());
    rml = match.suffix().str();
  }
  return out + rml;
}

class DeviceRemovedNoticeRenderTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
    AppRuntime::ShutdownUI();
    AppRuntime::InitializeUI();
    IAssetLocator::SetInstance(&locator_);
    ViewCatalog::ClearCache();
    ui::SetRenderInterface(&renderer_);
    ASSERT_TRUE(ui::Initialise());
    context_ = ui::CreateContext("device-removed-test", {400, 800}); // a phone
    ASSERT_NE(context_, nullptr);
    host_.Initialize(context_);
    ASSERT_TRUE(host_.RegisterWindowModel(context_));
    ASSERT_TRUE(DataModelHost::Instance().Register(context_, "shell", [this](ui::DataModelConstructor& ctor) {
      BindChatShellState(ctor, shell_);
      ctor.BindEventCallback("device_removed_relink", DataEvent([this] { events_.push_back("relink"); }));
      ctor.BindEventCallback("device_removed_ok", DataEvent([this] { events_.push_back("ok"); }));
    }));
  }

  void TearDown() override {
    DataModelHost::Instance().Clear();
    ui::RemoveContext("device-removed-test");
    ui::Shutdown();
    ui::SetRenderInterface(nullptr);
    AppRuntime::ShutdownUI();
    AppRuntime::InitializeUI();
    IAssetLocator::SetInstance(nullptr);
    ViewCatalog::ClearCache();
    LocalizationService::Instance().SetPreferredLanguage("en");
  }

  void Mount() {
    const std::string body = Translated(ViewCatalog::LoadBody("sidebar"));
    ASSERT_FALSE(body.empty());
    document_ = context_->LoadDocumentFromMemory(
        "<rml><head><style>body { display: block; } div, p, button, span { display: block; }</style></head><body>" +
        body + "</body></rml>");
    ASSERT_NE(document_, nullptr);
    document_->Show();
    context_->Update();
  }

  /** The visible tip cards, top to bottom. */
  std::vector<ui::Element*> Cards() {
    ui::ElementList all;
    document_->GetElementsByClassName(all, "sidebar-tip");
    std::vector<ui::Element*> shown;
    for (ui::Element* card : all) {
      if (card->IsVisible(true)) {
        shown.push_back(card);
      }
    }
    return shown;
  }

  static std::vector<ui::Element*> Buttons(ui::Element* card) {
    ui::ElementList all;
    card->GetElementsByTagName(all, "button");
    std::vector<ui::Element*> shown;
    for (ui::Element* button : all) {
      if (button->IsVisible(true)) {
        shown.push_back(button);
      }
    }
    return shown;
  }

  void Apply() {
    DataModelHost::Instance().Dirty("shell", "device_removed_notice");
    context_->Update();
  }

  void Tap(ui::Element* element) {
    ASSERT_NE(element, nullptr);
    element->DispatchEvent("click", {});
    context_->Update();
  }

  AssetsLocator locator_;
  NullRender renderer_;
  ShellHost host_;
  ChatShellState shell_;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  std::vector<std::string> events_;
};

TEST_F(DeviceRemovedNoticeRenderTest, NotShownWhileTheDeviceIsOnItsAccount) {
  Mount();
  EXPECT_TRUE(Cards().empty());
}

TEST_F(DeviceRemovedNoticeRenderTest, TheNoticeSaysWhatHappenedWithLinkAgainAndOk) {
  shell_.device_removed_notice = true;
  Mount();
  const auto cards = Cards();
  ASSERT_EQ(cards.size(), 1u);
  EXPECT_NE(cards[0]->GetInnerRML().find(
                "这台设备已经不在你的账号里了，收发可能不正常。从主设备重新关联它，或者在这台设备上重新开始。"),
            std::string::npos);
  const auto buttons = Buttons(cards[0]);
  ASSERT_EQ(buttons.size(), 2u);
  EXPECT_EQ(buttons[0]->GetInnerRML(), "重新关联");
  EXPECT_EQ(buttons[1]->GetInnerRML(), "知道了");
  Tap(buttons[0]);
  Tap(buttons[1]);
  EXPECT_EQ(events_, (std::vector<std::string>{"relink", "ok"}));
}

TEST_F(DeviceRemovedNoticeRenderTest, ItSitsAboveTheOtherCardsAndFollowsTheModel) {
  shell_.device_removed_notice = true;
  shell_.recovery_reminder = true;
  shell_.update_card = true;
  shell_.update_card_link = true;
  Mount();
  auto cards = Cards();
  ASSERT_EQ(cards.size(), 3u);
  EXPECT_NE(cards[0]->GetInnerRML().find("重新关联"), std::string::npos);
  EXPECT_LT(cards[0]->GetAbsoluteOffset(ui::BoxArea::Border).y, cards[1]->GetAbsoluteOffset(ui::BoxArea::Border).y);
  shell_.device_removed_notice = false; // "OK", or the device was linked again
  Apply();
  cards = Cards();
  ASSERT_EQ(cards.size(), 2u);
  EXPECT_EQ(cards[0]->GetInnerRML().find("重新关联"), std::string::npos);
}

TEST_F(DeviceRemovedNoticeRenderTest, EnglishWords) {
  LocalizationService::Instance().SetPreferredLanguage("en");
  shell_.device_removed_notice = true;
  Mount();
  const auto cards = Cards();
  ASSERT_EQ(cards.size(), 1u);
  EXPECT_NE(
      cards[0]->GetInnerRML().find("This device is no longer part of your account, so sending and receiving may "
                                   "not work. Link it again from your main device, or start over on this device."),
      std::string::npos);
  const auto buttons = Buttons(cards[0]);
  ASSERT_EQ(buttons.size(), 2u);
  EXPECT_EQ(buttons[0]->GetInnerRML(), "Link again");
  EXPECT_EQ(buttons[1]->GetInnerRML(), "OK");
}

} // namespace
} // namespace pbr
