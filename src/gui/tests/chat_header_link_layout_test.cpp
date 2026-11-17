// Layout on a 390dp phone with the real stylesheets: the chat header's link line under the name (direct / encrypted
// relay, the bubbles' route icons), and a settings feature name with its "?" (one centre line, a few dp apart).
#include "gui/chat/ChatDataModel.h"
#include "gui/shell/DataModelHost.h"
#include "foundation/i18n/LocalizationService.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

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

std::string AssetPath(const char* relative) {
  return std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative;
}

constexpr int kWidth = 390;
constexpr int kHeight = 844;

class ChatHeaderLinkLayoutTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    ASSERT_TRUE(ui::LoadFontFace(AssetPath("fonts/Inter-Regular.ttf"), /*fallback_face=*/true));
    context = ui::CreateContext("chat-header-link-test", {kWidth, kHeight});
    ASSERT_NE(context, nullptr);
    ASSERT_TRUE(DataModelHost::Instance().Register(
        context, "chat", [this](ui::DataModelConstructor& ctor) { BindChatScreenState(ctor, state); }));
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext("chat-header-link-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }
  void Load(const std::string& inner, const char* wrapper_classes) {
    const std::string rml = "<rml><head><style>" + ReadAll(AssetPath("themes/foundation.rcss")) +
                            ReadAll(AssetPath("themes/components.rcss")) + ReadAll(AssetPath("themes/colors-light.rcss")) +
                            "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
                            "<div style=\"position: absolute; top: 0; left: 0; width: " +
                            std::to_string(kWidth) + "dp; height: " + std::to_string(kHeight) +
                            "dp;\"><div class=\"shell-layer shell-layer-base shell-layer-compact\">" + inner +
                            "</div></div></body></rml>";
    (void)wrapper_classes;
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    context->Update();
  }
  void ShowChat() {
    state.use_messages_layout = true;
    state.has_turns = true;
    state.thread_title = "Ann";
    state.thread_subtitle = "Long encryption subtitle that stays on the desktop";
    Load("<div class=\"shell-chat-overlay\" id=\"shell-chat-overlay\"><div class=\"shell-pane-body\" "
         "id=\"pane-body-chat\">" +
             LocalizationService::Instance().LocalizeText(ReadAll(AssetPath("views/chat.rml"))) + "</div></div>",
         "");
  }
  struct Edges {
    float left, top, right, bottom;
    float MidY() const { return (top + bottom) / 2; }
  };
  static Edges BorderEdges(ui::Element* element) {
    const ui::Vector2f at = element->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f size = element->GetBox().GetSize(ui::BoxArea::Border);
    return {at.x, at.y, at.x + size.x, at.y + size.y};
  }
  ui::Element* First(const char* cls) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    return found.empty() ? nullptr : found[0];
  }

  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  ChatScreenState state;
};

TEST_F(ChatHeaderLinkLayoutTest, ThePhoneHeaderShowsTheRouteIconAndWordsUnderTheName) {
  state.show_peer_link = true;
  state.peer_link_direct = true;
  state.peer_link_status = "Direct";
  ShowChat();
  ui::Element* path = First("chat-peer-path");
  ui::Element* title = First("chat-header-title");
  ui::Element* icon = First("route-icon");
  ASSERT_NE(path, nullptr);
  ASSERT_NE(title, nullptr);
  ASSERT_NE(icon, nullptr);
  const Edges p = BorderEdges(path);
  EXPECT_GT(p.bottom - p.top, 8.f) << "the line is laid out on a phone";
  EXPECT_GE(p.top, BorderEdges(title).bottom - 1.f) << "under the name";
  EXPECT_NE(std::string(icon->GetAttribute<ui::String>("src", "").c_str()).find("route-direct.svg"),
            std::string::npos);
  EXPECT_EQ(BorderEdges(First("chat-header-subtitle")).bottom - BorderEdges(First("chat-header-subtitle")).top, 0.f)
      << "the long encryption subtitle stays off the phone";
}

TEST_F(ChatHeaderLinkLayoutTest, RelayUsesTheRelayIconAndAGroupShowsNoLine) {
  state.show_peer_link = true;
  state.peer_link_via_relay = true;
  state.peer_link_status = "Encrypted relay";
  ShowChat();
  ui::ElementList icons;
  document->GetElementsByClassName(icons, "route-icon");
  ui::Element* icon = nullptr; // the direct one is in the page but not shown
  for (ui::Element* candidate : icons) {
    if (candidate->IsVisible()) {
      icon = candidate;
    }
  }
  ASSERT_NE(icon, nullptr);
  EXPECT_NE(std::string(icon->GetAttribute<ui::String>("src", "").c_str()).find("route-relay.svg"),
            std::string::npos);
  EXPECT_GT(BorderEdges(First("chat-peer-path")).bottom - BorderEdges(First("chat-peer-path")).top, 8.f);
}

TEST_F(ChatHeaderLinkLayoutTest, AGroupInACallShowsNoLineOnThePhone) {
  state.show_peer_link = true;
  state.peer_link_ready = true;
  state.peer_link_status = "In a call";
  state.thread_is_group = true;
  ShowChat();
  const Edges group = BorderEdges(First("chat-peer-path"));
  EXPECT_EQ(group.bottom - group.top, 0.f) << "a group's phone header is unchanged";
}

TEST_F(ChatHeaderLinkLayoutTest, AFeatureNameAndItsQuestionMarkShareACentreLineAFewDpApart) {
  Load("<div class=\"settings-section settings-section--grouped settings-mock stack\">"
       "<p class=\"text group-head\">Above</p>"
       "<div class=\"settings-q-head settings-q-head--name row\"><p class=\"text group-head group-head--name\">Recovery "
       "phrase</p><button class=\"settings-q-btn\" type=\"button\"><svg class=\"settings-q-icon\" "
       "src=\"../icons/v2/help-circle.svg\" width=\"20\" height=\"20\"></svg></button></div></div>",
       "");
  ui::Element* name = First("group-head--name");
  ui::Element* button = First("settings-q-btn");
  ui::Element* icon = First("settings-q-icon");
  ASSERT_NE(name, nullptr);
  ASSERT_NE(button, nullptr);
  ASSERT_NE(icon, nullptr);
  const Edges n = BorderEdges(name);
  const Edges b = BorderEdges(button);
  // The 20dp icon is centred in the 44dp button (the file may not load here, so measure the button's centre).
  const float icon_left = b.left + (b.right - b.left - 20.f) / 2;
  EXPECT_NEAR(n.MidY(), b.MidY(), 1.f) << "the heading and the icon share a centre line";
  EXPECT_GE(icon_left - n.right, 4.f);
  EXPECT_LE(icon_left - n.right, 8.f) << "the icon sits a few dp after the heading";
  EXPECT_GE(b.bottom - b.top, 43.f) << "the hit area stays 44dp";
  EXPECT_GE(b.right - b.left, 43.f);
}

} // namespace
} // namespace pbr
