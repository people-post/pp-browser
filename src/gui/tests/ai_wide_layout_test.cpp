// The AI section in the wide layout (design step 3, page C): ai_list_wide.rml (the phone's "Conversation history" drawer as
// the list column) over the real shell data model and stylesheets at 768 / 900 / 1280, the shell layout that gives AI its
// list column while Chats keeps only people and groups, and the phone's AI home look (no heading) in the wide root.
#include "foundation/i18n/LocalizationService.h"
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/ChatScreenState.h"
#include "gui/shell/DataModelHost.h"
#include "gui/shell/ShellLayoutRml.h"
#include "lib/ui/ShellLayout.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
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

std::string Asset(const char* relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

bool Has(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

constexpr const char* kContext = "ai-wide-layout-test";
constexpr int kRail = 64;
constexpr int kList = 320;

class AiWideLayoutTest : public testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    ui::SetRenderInterface(&render_);
    ASSERT_TRUE(ui::Initialise());
    ASSERT_TRUE(ui::LoadFontFace(std::string(PP_BROWSER_ASSETS_DIR) + "/fonts/LatoLatin-Regular.ttf"));
    ASSERT_TRUE(ui::LoadFontFace(std::string(PP_BROWSER_ASSETS_DIR) + "/fonts/Inter-Regular.ttf", true));
    context_ = ui::CreateContext(kContext, {1280, 800});
    ASSERT_NE(context_, nullptr);
    for (const char* title : {"Kyoto in three days", "What is in the news today?"}) {
      SessionDisplayRow row;
      row.id = (std::string("ai-") + title).c_str();
      row.title = title;
      row.preview = "Thanks, that helps a lot";
      row.kind = "ai";
      row.avatar_letter = "K";
      row.avatar_tone = 2;
      row.date_label = "Thursday";
      shell_.ai_sessions.push_back(row);
    }
    shell_.ai_sessions[1].active = true;
    shell_.ai_sessions_count_label = "2 conversations";
    ASSERT_TRUE(DataModelHost::Instance().Register(context_, "shell", [this](ui::DataModelConstructor& ctor) {
      BindChatShellState(ctor, shell_);
      for (const char* name : {"new_chat", "select_thread", "ai_sessions_search_changed", "toggle_archived_ai",
                               "ai_sessions_scroll", "device_removed_relink", "device_removed_ok"}) {
        ctor.BindEventCallback(name, [this, name](ui::DataModelHandle, ui::Event&, const ui::VariantList& args) {
          calls_.push_back(std::string(name) +
                           (args.empty() ? "" : ":" + std::string(args[0].Get<ui::String>().c_str())));
        });
      }
    }));
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    ui::RemoveContext(kContext);
    ui::Shutdown();
    ui::SetRenderInterface(nullptr);
  }

  void Load(const int window_dp) {
    const int list = std::max(280, std::min(kList, window_dp - kRail - 420));
    const std::string page = LocalizationService::Instance().LocalizeText(Asset("views/ai_list_wide.rml"));
    const std::string rml =
        "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
        Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") +
        "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
        "<div class=\"shell-body shell-body--wide\" style=\"position: absolute; top: 0; left: 0; width: " +
        std::to_string(window_dp) +
        "dp; height: 700dp;\"><div class=\"shell-layer shell-layer-base\">"
        "<div id=\"list\" class=\"shell-pane shell-pane-secondary\" style=\"position: absolute; left: " +
        std::to_string(kRail) + "dp; width: " + std::to_string(list) +
        "dp; height: 700dp;\"><div class=\"shell-pane-body\">" + page +
        "</div></div>"
        "<div id=\"home\" class=\"home-header\">PP</div></div></div></body></rml>";
    document_ = context_->LoadDocumentFromMemory(rml);
    ASSERT_NE(document_, nullptr);
    document_->Show();
    context_->Update();
    context_->Update();
  }
  struct Box {
    float left, top, right, bottom;
    float W() const { return right - left; }
    float H() const { return bottom - top; }
  };
  static Box BoxOf(ui::Element* element) {
    const ui::Vector2f at = element->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f size = element->GetBox().GetSize(ui::BoxArea::Border);
    return {at.x, at.y, at.x + size.x, at.y + size.y};
  }
  ui::ElementList All(const char* selector) {
    ui::ElementList found;
    document_->QuerySelectorAll(found, selector);
    ui::ElementList drawn; // data-for leaves a hidden template element after its rows
    for (ui::Element* element : found) {
      if (BoxOf(element).H() > 0.f) {
        drawn.push_back(element);
      }
    }
    return drawn;
  }

  NullRender render_;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ChatShellState shell_;
  std::vector<std::string> calls_;
};

TEST_F(AiWideLayoutTest, TheListIsThePhonesConversationHistoryInsideTheColumnAtEveryWidth) {
  for (const int window : {768, 900, 1280}) {
    SCOPED_TRACE(window);
    Load(window);
    const Box column = BoxOf(All("#list")[0]);
    const ui::ElementList rows = All(".sidebar-session");
    ASSERT_EQ(rows.size(), 2u);
    for (ui::Element* row : rows) {
      EXPECT_GE(BoxOf(row).H(), 60.f) << "a click target well over 44dp";
      EXPECT_GE(BoxOf(row).left, column.left);
      EXPECT_LE(BoxOf(row).right, column.right);
      EXPECT_TRUE(row->HasAttribute("session-id")) << "the Chats list's right-click menu finds the row by it";
      EXPECT_FALSE(row->IsClassSet("session-swipe-shown"));
    }
    EXPECT_FALSE(rows[0]->IsClassSet("sidebar-session-active"));
    EXPECT_TRUE(rows[1]->IsClassSet("sidebar-session-active")) << "the open conversation is marked";
    // The tile: the title's first character in a colour, 48dp; the title and last message beside it.
    ui::Element* tile = All(".chat-row-avatar")[0];
    EXPECT_FLOAT_EQ(BoxOf(tile).W(), 48.f);
    EXPECT_LT(BoxOf(tile).right, BoxOf(All(".sidebar-session-title")[0]).left + 0.5f);
    // Header: the title and the new-conversation plus, search under it, the count line at the foot.
    ui::Element* title = All(".sidebar-header-title")[0];
    EXPECT_EQ(std::string(title->GetInnerRML().c_str()), "Chat history");
    EXPECT_LE(BoxOf(All(".sidebar-new-chat")[0]).right, column.right);
    EXPECT_GT(BoxOf(All(".chat-search")[0]).top, BoxOf(title).bottom - 1.f);
    ui::Element* count = All(".ai-list-count")[0];
    EXPECT_GT(BoxOf(count).top, BoxOf(rows[1]).bottom);
    EXPECT_LE(BoxOf(count).bottom, column.bottom + 0.5f);
    EXPECT_EQ(All(".session-swipe").size(), 0u) << "the phone's swipe buttons are not in the wide list";
  }
}

TEST_F(AiWideLayoutTest, ARowOpensItsConversationAndThePlusStartsANewOne) {
  Load(900);
  const ui::ElementList rows = All(".sidebar-session");
  ASSERT_EQ(rows.size(), 2u);
  rows[0]->DispatchEvent(ui::EventId::Click, {});
  All(".sidebar-new-chat")[0]->DispatchEvent(ui::EventId::Click, {});
  ASSERT_EQ(calls_.size(), 2u);
  EXPECT_EQ(calls_[0], "select_thread:ai-Kyoto in three days");
  EXPECT_EQ(calls_[1], "new_chat");
}

// A linked device its account took off its list (DeviceRemovedNotice): the AI column, the desktop's Home, shows the
// Chats list's card at its top, above the conversations, inside the column, with "Link again" and "OK" reaching the
// controller's handlers; not while searching, and gone once the model says so.
TEST_F(AiWideLayoutTest, TheDeviceRemovedNoticeIsTheCardAtTheTopOfTheList) {
  context_->ActivateTheme("light", true); // the colour file's rules sit under @media (theme: light)
  shell_.device_removed_notice = true;
  for (const int window : {768, 1280}) {
    SCOPED_TRACE(window);
    Load(window);
    const Box column = BoxOf(All("#list")[0]);
    const ui::ElementList cards = All(".sidebar-tip");
    ASSERT_EQ(cards.size(), 1u);
    const Box card = BoxOf(cards[0]);
    EXPECT_GE(card.left, column.left + 15.5f) << "the list's 8dp padding and the card's 8dp margin";
    EXPECT_LE(card.right, column.right - 15.5f);
    EXPECT_GT(card.top, BoxOf(All(".chat-search")[0]).bottom - 0.5f) << "under the search field";
    EXPECT_LT(card.bottom, BoxOf(All(".sidebar-session")[0]).top + 0.5f) << "above the conversations";
    EXPECT_FLOAT_EQ(cards[0]->GetComputedValues().border_top_left_radius(), 14.f) << "the tip cards' corner";
    EXPECT_GT(cards[0]->GetComputedValues().background_color().alpha, 0) << "a filled card, as on the Chats list";
    ui::ElementList body;
    cards[0]->GetElementsByClassName(body, "sidebar-tip-body");
    ASSERT_EQ(body.size(), 1u);
    EXPECT_EQ(std::string(body[0]->GetInnerRML().c_str()), Tr("device_removed.body"));
    ui::ElementList buttons;
    cards[0]->GetElementsByTagName(buttons, "button");
    ASSERT_EQ(buttons.size(), 2u);
    EXPECT_EQ(std::string(buttons[0]->GetInnerRML().c_str()), Tr("device_removed.relink"));
    EXPECT_EQ(std::string(buttons[1]->GetInnerRML().c_str()), Tr("device_removed.ok"));
    for (ui::Element* button : buttons) {
      EXPECT_GT(BoxOf(button).H(), 0.f);
      EXPECT_LE(BoxOf(button).right, card.right);
    }
    document_->Close();
    context_->Update();
  }
  Load(900);
  ui::ElementList buttons;
  All(".sidebar-tip")[0]->GetElementsByTagName(buttons, "button");
  ASSERT_EQ(buttons.size(), 2u);
  buttons[0]->DispatchEvent(ui::EventId::Click, {});
  buttons[1]->DispatchEvent(ui::EventId::Click, {});
  EXPECT_EQ(calls_, (std::vector<std::string>{"device_removed_relink", "device_removed_ok"}));

  document_->Close();
  context_->Update();
  shell_.ai_sessions_query = "kyoto"; // searching: the list is the matches only
  Load(900);
  EXPECT_EQ(All(".sidebar-tip").size(), 0u);
  document_->Close();
  context_->Update();
  shell_.ai_sessions_query = "";
  shell_.device_removed_notice = false; // "OK", or the device was linked again
  Load(900);
  EXPECT_EQ(All(".sidebar-tip").size(), 0u);
}

// The phone's AI home has no "PP" heading above the mascot; in the wide root the older heading is hidden as well.
TEST_F(AiWideLayoutTest, TheAiHomeHasNoHeadingInTheWideRoot) {
  Load(900);
  ui::Element* heading = document_->GetElementById("home");
  ASSERT_NE(heading, nullptr);
  EXPECT_EQ(heading->GetComputedValues().display(), ui::Style::Display::None);
}

// AI gets the list column in the wide layout; Chats' list is the people and groups one; the phone's AI tab has no list.
TEST(AiWideShellTest, AiHasItsConversationListBesideItAndChatsKeepsThePeopleList) {
  ShellState state;
  state.layout_mode = LayoutMode::Expanded;
  state.nav_tab = NavTab::Home;
  const std::string ai = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(ai, "shell-nav-content-mount"));
  EXPECT_TRUE(Has(ai, "id=\"pane-body-ai_list\""));
  EXPECT_FALSE(Has(ai, "id=\"pane-body-sidebar\""));
  state.nav_tab = NavTab::Sessions;
  const std::string chats = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(chats, "id=\"pane-body-sidebar\""));
  EXPECT_FALSE(Has(chats, "ai_list"));
  state.layout_mode = LayoutMode::Compact;
  state.nav_tab = NavTab::Home;
  EXPECT_FALSE(Has(ShellLayoutRml(state, false, "").ShellRoot(), "ai_list")) << "the phone's AI page has its drawer";

  // The two lists are fed by different rows: the people list binds `sessions`, the AI list `ai_sessions`, never both.
  const std::string sidebar = Asset("views/sidebar.rml");
  const std::string ai_list = Asset("views/ai_list_wide.rml");
  EXPECT_TRUE(Has(sidebar, "session : sessions\""));
  EXPECT_FALSE(Has(sidebar, "ai_sessions"));
  EXPECT_TRUE(Has(ai_list, "session : ai_sessions\""));
  EXPECT_FALSE(Has(ai_list, "session : sessions\""));
}

} // namespace
} // namespace pbr
