// The Chats list's "new version" card as drawn: the real sidebar view (through the real ViewCatalog) over the real shell
// data model. The card looks like the recovery-phrase card and sits under it; Update only with a link; both lines
// and buttons follow the model; the card's two events reach the handlers the controller binds.
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

class UpdateCardRenderTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    AppRuntime::ShutdownUI();
    AppRuntime::InitializeUI();
    IAssetLocator::SetInstance(&locator_);
    ViewCatalog::ClearCache();
    ui::SetRenderInterface(&renderer_);
    ASSERT_TRUE(ui::Initialise());
    context_ = ui::CreateContext("update-card-test", {400, 800});
    ASSERT_NE(context_, nullptr);
    host_.Initialize(context_);
    ASSERT_TRUE(host_.RegisterWindowModel(context_));
    ASSERT_TRUE(DataModelHost::Instance().Register(context_, "shell", [this](ui::DataModelConstructor& ctor) {
      BindChatShellState(ctor, shell_);
      ctor.BindEventCallback("update_card_open", DataEvent([this] { events_.push_back("open"); }));
      ctor.BindEventCallback("update_card_later", DataEvent([this] { events_.push_back("later"); }));
    }));
  }

  void TearDown() override {
    DataModelHost::Instance().Clear();
    ui::RemoveContext("update-card-test");
    ui::Shutdown();
    ui::SetRenderInterface(nullptr);
    AppRuntime::ShutdownUI();
    AppRuntime::InitializeUI();
    IAssetLocator::SetInstance(nullptr);
    ViewCatalog::ClearCache();
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
    DataModelHost::Instance().Dirty("shell", "update_card");
    DataModelHost::Instance().Dirty("shell", "update_card_link");
    DataModelHost::Instance().Dirty("shell", "update_card_note_shown");
    DataModelHost::Instance().Dirty("shell", "update_card_note");
    DataModelHost::Instance().Dirty("shell", "update_card_held");
    DataModelHost::Instance().Dirty("shell", "recovery_reminder");
    context_->Update();
  }

  /** The tap's click, delivered to the element (the page here has no stylesheet, so nothing to hit-test). */
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

TEST_F(UpdateCardRenderTest, NoCardUntilAVersionIsAnnounced) {
  Mount();
  EXPECT_TRUE(Cards().empty());
}

TEST_F(UpdateCardRenderTest, WithALinkTheCardHasUpdateAndLater) {
  shell_.update_card = true;
  shell_.update_card_link = true;
  Mount();
  const auto cards = Cards();
  ASSERT_EQ(cards.size(), 1u);
  EXPECT_NE(cards[0]->GetInnerRML().find("A new version of PP is available. Update for the best experience."),
            std::string::npos);
  const auto buttons = Buttons(cards[0]);
  ASSERT_EQ(buttons.size(), 2u);
  EXPECT_EQ(buttons[0]->GetInnerRML(), "Update");
  EXPECT_EQ(buttons[1]->GetInnerRML(), "Later");
  Tap(buttons[0]);
  Tap(buttons[1]);
  EXPECT_EQ(events_, (std::vector<std::string>{"open", "later"}));
}

TEST_F(UpdateCardRenderTest, WithoutALinkTheCardKeepsItsLineAndOnlyLater) {
  shell_.update_card = true;
  shell_.update_card_link = false;
  Mount();
  const auto cards = Cards();
  ASSERT_EQ(cards.size(), 1u);
  EXPECT_NE(
      cards[0]->GetInnerRML().find("A new version of PP is available. Please update it from where you installed PP."),
      std::string::npos);
  const auto buttons = Buttons(cards[0]);
  ASSERT_EQ(buttons.size(), 1u);
  EXPECT_EQ(buttons[0]->GetInnerRML(), "Later");
}

// T14: held up while the org's relay refuses this app: the card's own words and its Update button, no Later.
TEST_F(UpdateCardRenderTest, AHeldCardHasUpdateAndNoLater) {
  shell_.update_card = true;
  shell_.update_card_link = true;
  shell_.update_card_held = true;
  Mount();
  const auto cards = Cards();
  ASSERT_EQ(cards.size(), 1u);
  EXPECT_NE(cards[0]->GetInnerRML().find("A new version of PP is available. Update for the best experience."),
            std::string::npos)
      << "the card's own line, unchanged";
  const auto buttons = Buttons(cards[0]);
  ASSERT_EQ(buttons.size(), 1u);
  EXPECT_EQ(buttons[0]->GetInnerRML(), "Update");
  shell_.update_card_held = false;
  Apply();
  EXPECT_EQ(Buttons(Cards()[0]).size(), 2u) << "Later comes back once it is not held";
}

TEST_F(UpdateCardRenderTest, TheRelaysNoteIsASecondLineUnderTheAppsOwnAndShownAsPlainText) {
  shell_.update_card = true;
  shell_.update_card_link = true;
  shell_.update_card_note_shown = true;
  shell_.update_card_note = "<b>Fixes</b> calls & <a href=\"https://x.test\">more</a>";
  Mount();
  const auto cards = Cards();
  ASSERT_EQ(cards.size(), 1u);
  EXPECT_NE(cards[0]->GetInnerRML().find("A new version of PP is available. Update for the best experience."),
            std::string::npos)
      << "the app's own line stays";
  ui::ElementList notes;
  cards[0]->GetElementsByClassName(notes, "sidebar-tip-note");
  ASSERT_EQ(notes.size(), 1u);
  EXPECT_TRUE(notes[0]->IsVisible(true));
  ui::ElementList markup;
  document_->GetElementsByTagName(markup, "b");
  document_->GetElementsByTagName(markup, "a");
  EXPECT_TRUE(markup.empty()) << "the note's markup characters are text, not elements";
}

TEST_F(UpdateCardRenderTest, WithoutANoteThereIsNoSecondLine) {
  shell_.update_card = true;
  shell_.update_card_link = true;
  Mount();
  const auto cards = Cards();
  ASSERT_EQ(cards.size(), 1u);
  ui::ElementList notes;
  cards[0]->GetElementsByClassName(notes, "sidebar-tip-note");
  for (ui::Element* note : notes) {
    EXPECT_FALSE(note->IsVisible(true));
  }
}

TEST_F(UpdateCardRenderTest, WhenBothAreDueTheRecoveryCardIsFirstAndTheUpdateCardBelowIt) {
  shell_.recovery_reminder = true;
  shell_.update_card = true;
  shell_.update_card_link = true;
  Mount();
  const auto cards = Cards();
  ASSERT_EQ(cards.size(), 2u);
  EXPECT_NE(cards[0]->GetInnerRML().find("recovery phrase"), std::string::npos);
  EXPECT_NE(cards[1]->GetInnerRML().find("new version"), std::string::npos);
  EXPECT_LT(cards[0]->GetAbsoluteOffset(ui::BoxArea::Border).y, cards[1]->GetAbsoluteOffset(ui::BoxArea::Border).y);
}

TEST_F(UpdateCardRenderTest, TheCardFollowsTheModelAndNeverShowsOverASearchOrTheArchive) {
  shell_.update_card = true;
  shell_.update_card_link = true;
  Mount();
  ASSERT_EQ(Cards().size(), 1u);
  shell_.update_card = false;
  Apply();
  EXPECT_TRUE(Cards().empty());
  shell_.update_card = true;
  shell_.showing_archived = true;
  DataModelHost::Instance().Dirty("shell", "showing_archived");
  Apply();
  EXPECT_TRUE(Cards().empty());
  shell_.showing_archived = false;
  shell_.sessions_query = "li";
  DataModelHost::Instance().Dirty("shell", "showing_archived");
  DataModelHost::Instance().Dirty("shell", "sessions_query");
  Apply();
  EXPECT_TRUE(Cards().empty()) << "a search is on";
  shell_.sessions_query = "";
  DataModelHost::Instance().Dirty("shell", "sessions_query");
  Apply();
  EXPECT_EQ(Cards().size(), 1u) << "back once the search is cleared";
}

TEST_F(UpdateCardRenderTest, TheLinesAreTranslated) {
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("compat.update_card.body"), "发现新版本，建议更新以获得更好的体验。");
  EXPECT_EQ(Tr("compat.update_card.body_nolink"), "发现新版本，请前往你安装 PP 的应用商店更新。");
  EXPECT_EQ(Tr("compat.update_card.action"), "更新");
  EXPECT_EQ(Tr("compat.update_card.later"), "以后再说");
  LocalizationService::Instance().SetPreferredLanguage("en");
}

} // namespace
} // namespace pbr
