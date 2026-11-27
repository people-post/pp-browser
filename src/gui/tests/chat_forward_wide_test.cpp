// Forwarding on the desktop (DEV/PLAN-message-forward-text.md, step 5): choosing messages in the wide chat (the top bar
// "N selected / Cancel" and the bottom Forward bar stay inside the conversation column) and the "Forward to" card
// (people_picker_wide.rml) staying inside its window. The real views and stylesheets, the real shell markup; the
// catalog is loaded in each language directly, not through the app's language choice.
#include "feature/presenters/MessageForward.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/chat/ChatDataModel.h"
#include "gui/contacts/PeoplePickerController.h"
#include "gui/shell/DataModelHost.h"
#include "gui/shell/ShellLayoutRml.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
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

std::string Asset(const std::string& relative) {
  std::ifstream in(std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

/** Puts `body` into the empty element `id` of `root`. */
void Fill(std::string& root, const std::string& id, const std::string& body) {
  const std::string empty = "id=\"" + id + "\"></div>";
  const size_t at = root.find(empty);
  if (at != std::string::npos) {
    root.replace(at, empty.size(), "id=\"" + id + "\">" + body + "</div>");
  }
}

struct Window {
  int w;
  int h;
};
constexpr Window kWindows[] = {{1280, 720}, {1024, 640}, {900, 600}};
constexpr float kMargin = 12.f;
constexpr float kTitlebar = 36.f;
constexpr float kStatusbar = 24.f;

struct Box {
  float left, top, right, bottom;
  float W() const { return right - left; }
  float H() const { return bottom - top; }
};

Box BoxOf(ui::Element* element) {
  const ui::Vector2f at = element->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Vector2f size = element->GetBox().GetSize(ui::BoxArea::Border);
  return {at.x, at.y, at.x + size.x, at.y + size.y};
}

bool Drawn(ui::Element* element) {
  return element->IsVisible(/*include_ancestors=*/true) && BoxOf(element).W() > 0.f;
}

class ForwardWideTest : public testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    ui::SetRenderInterface(&render_);
    initialized_ = ui::Initialise();
    ASSERT_TRUE(initialized_);
    ASSERT_TRUE(ui::LoadFontFace(std::string(PP_BROWSER_ASSETS_DIR) + "/fonts/LatoLatin-Regular.ttf"));
    ASSERT_TRUE(ui::LoadFontFace(std::string(PP_BROWSER_ASSETS_DIR) + "/fonts/Inter-Regular.ttf", true));
    ASSERT_TRUE(ui::LoadFontFace(std::string(PP_BROWSER_ASSETS_DIR) + "/fonts/NotoSansSC-Medium.otf", true));
  }
  void TearDown() override {
    if (document_) {
      document_->Close();
      document_ = nullptr;
    }
    DataModelHost::Instance().Clear();
    if (context_) {
      ui::RemoveContext("forward-wide-test");
      context_ = nullptr;
    }
    if (initialized_) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
    LocalizationService::Instance().SetPreferredLanguage("en");
  }

  /** A fresh context of the window's size (the previous one is gone). */
  void NewContext(const Window& window) {
    if (document_) {
      document_->Close();
      document_ = nullptr;
    }
    DataModelHost::Instance().Clear();
    if (context_) {
      ui::RemoveContext("forward-wide-test");
    }
    context_ = ui::CreateContext("forward-wide-test", {window.w, window.h});
    ASSERT_NE(context_, nullptr);
  }

  /** The shell's markup for `state` in the window, with the page bodies filled, and the real stylesheets. */
  void LoadShell(const Window& window, const ShellState& state, const std::function<void(std::string&)>& fill) {
    std::string root = ShellLayoutRml(state, false, "").ShellRoot();
    root = std::regex_replace(root, std::regex(" data-model=\"window\""), "");
    fill(root);
    const std::string rml =
        "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
        Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") +
        "</style></head><body id=\"body\" class=\"shell-body shell-body--wide" +
        std::string(state.overlay_stack.empty() ? "" : " shell-body--modal") +
        "\" style=\"padding: 0; font-family: Inter; font-size: 15dp; position: absolute; top: 0; left: 0; width: " +
        std::to_string(window.w) + "dp; height: " + std::to_string(window.h) +
        "dp;\"><div id=\"shell-titlebar\" class=\"shell-titlebar\"></div><div id=\"shell-root\" class=\"shell-root\" "
        "style=\"top: " +
        std::to_string(static_cast<int>(kTitlebar)) + "dp; bottom: " + std::to_string(static_cast<int>(kStatusbar)) +
        "dp;\">" + LocalizationService::Instance().LocalizeText(root) +
        "</div><div id=\"shell-statusbar\" class=\"shell-statusbar\"></div></body></rml>";
    document_ = context_->LoadDocumentFromMemory(rml);
    ASSERT_NE(document_, nullptr);
    document_->Show();
    for (const char* part : {"shell-modal-scrim-7", "shell-modal-stage-7"}) {
      if (ui::Element* element = document_->GetElementById(part)) {
        element->SetProperty("top", "-36dp"); // what ShellHost::ApplySafeAreaLayout does for a modal layer
        element->SetProperty("bottom", "-24dp");
      }
    }
    for (int i = 0; i < 3; ++i) {
      context_->Update();
    }
  }

  ui::ElementList All(const char* cls) {
    ui::ElementList found;
    document_->GetElementsByClassName(found, cls);
    return found;
  }
  ui::Element* First(const char* cls) {
    ui::ElementList found = All(cls);
    return found.empty() ? nullptr : found[0];
  }

  NullRender render_;
  bool initialized_ = false;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
};

MessageDisplayRow ChooseRow(const std::string& id, const bool mine, const bool ok, const bool ticked) {
  MessageDisplayRow row;
  row.message_id = id.c_str();
  row.row_class = mine ? "message-row-user" : "message-row-peer";
  row.content_rml =
      "<div class=\"bubble bubble-user\"><p class=\"bubble-text\">A message of some length, to be sent "
      "on</p></div>";
  row.has_actions = true;
  row.forward_mode = true;
  row.forward_ok = ok;
  row.forward_ticked = ticked;
  return row;
}

// Choosing messages in the wide chat: the title bar ("N selected", Cancel) takes the header's place and the Forward
// button the composer's, both inside the conversation column with room to spare, in either language.
TEST_F(ForwardWideTest, WideSelectionModeKeepsTheTopAndBottomBarsInsideTheChatColumn) {
  for (const char* language : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(language);
    for (const Window& window : kWindows) {
      const std::string where = std::string(language) + " " + std::to_string(window.w) + "x" + std::to_string(window.h);
      NewContext(window);
      ChatScreenState chat;
      chat.use_messages_layout = true;
      chat.has_turns = true;
      chat.thread_title = "Ann Lee";
      chat.forward_selecting = true;
      chat.forward_can_next = true;
      chat.forward_count_label =
          LocalizationService::Instance().Tr("chat.forward.selected_count", {{"n", "30"}}).c_str();
      chat.messages = {ChooseRow("m1", true, true, true), ChooseRow("m2", false, true, false),
                       ChooseRow("m3", false, false, false)};
      ASSERT_TRUE(DataModelHost::Instance().Register(
          context_, "chat", [&chat](ui::DataModelConstructor& ctor) { BindChatScreenState(ctor, chat); }));
      ShellState state;
      state.layout_mode = LayoutMode::Expanded;
      state.nav_tab = NavTab::Sessions;
      state.panes.push_back(
          {.spec = {.key = "chat", .rml_path = "views/chat.rml", .provides_composer = true}, .id = 1});
      state.primary_pane_key = "chat";
      LoadShell(window, state, [](std::string& root) {
        Fill(root, "pane-body-chat", Asset("views/chat_wide.rml"));
        Fill(root, "pane-composer-chat", Asset("views/composer_wide.rml"));
      });
      ui::Element* column = document_->QuerySelector("#pane-chat");
      ASSERT_NE(column, nullptr) << where;
      const Box col = BoxOf(column);
      ASSERT_GT(col.W(), 300.f) << where;

      ui::Element* title = First("chat-forward-title");
      ui::Element* cancel = First("chat-forward-cancel");
      ui::Element* next = First("chat-forward-next");
      ASSERT_TRUE(title && cancel && next) << where;
      for (ui::Element* element : {title, cancel, next}) {
        ASSERT_TRUE(Drawn(element)) << where;
        const Box b = BoxOf(element);
        EXPECT_GE(b.left, col.left + kMargin) << where;
        EXPECT_LE(b.right, col.right - kMargin) << where;
        EXPECT_GE(b.top, col.top + 0.f) << where;
        EXPECT_LE(b.bottom, col.bottom - kMargin) << where;
      }
      EXPECT_LE(BoxOf(title).right, BoxOf(cancel).left + 0.5f) << "the count never runs under Cancel: " << where;
      EXPECT_LE(BoxOf(cancel).bottom, BoxOf(First("chat-header")).bottom + 0.5f)
          << "the top bar is in the header: " << where;
      EXPECT_GE(BoxOf(cancel).H(), 36.f) << where;
      EXPECT_GE(BoxOf(next).H(), 44.f) << where;
      // The composer is gone while choosing, the header's own buttons too; the bar and the rows keep out of each other.
      for (ui::Element* gone : All("prompt-composer")) {
        EXPECT_FALSE(Drawn(gone)) << where;
      }
      for (ui::Element* gone : All("chat-header-call-btn")) {
        EXPECT_FALSE(Drawn(gone)) << where;
      }
      // A tick on the two rows that can go, none on the third; no hover bar while choosing.
      EXPECT_EQ(All("message-forward-check").size(), 3u) << "one per row in the mode (the third is hidden): " << where;
      size_t ticks_drawn = 0;
      for (ui::Element* tick : All("message-forward-check")) {
        ticks_drawn += Drawn(tick) ? 1u : 0u;
      }
      EXPECT_EQ(ticks_drawn, 2u) << where;
    }
  }
}

std::vector<ForwardTarget> Targets() {
  std::vector<ForwardTarget> targets;
  const char* names[] = {"Alexandra Montgomery-Featherstonehaugh",
                         "Bartholomew Oyelaran-Whitcombe",
                         "Catherine Dubois",
                         "Dmitri Alexandrovich Romanovsky",
                         "Evangeline Rodriguez-Hernandez",
                         "李云龙的朋友们大家庭",
                         "Frederick Nakamura",
                         "Gwendolyn Papadopoulos-Smith",
                         "Harold",
                         "Isabella Fitzgerald-Okonkwo",
                         "Jonathan Weatherby",
                         "Katarzyna Wisniewska-Kowalczyk"};
  int64_t at = 100;
  for (const char* name : names) {
    ForwardTarget target;
    target.thread_id = "t" + std::to_string(at);
    target.contact_id = "c" + std::to_string(at);
    target.title = name;
    target.subtitle = "a.long.address@example.invalid";
    target.updated_at = at--;
    targets.push_back(target);
  }
  ForwardTarget club;
  club.thread_id = "g1";
  club.title = "The Saturday Morning Cycling and Coffee Club of the Old Town";
  club.group = true;
  targets.push_back(club);
  return targets;
}

// The "Forward to" card: a modal frame in the window, 12dp clear of every window edge at the three window sizes,
// and its Cancel / Send, search, preview and rows inside it with 12dp to spare, English and Chinese.
TEST_F(ForwardWideTest, PeoplePickerWideKeepsEveryButtonAndRowInsideTheCard) {
  for (const char* language : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(language);
    for (const Window& window : kWindows) {
      const std::string where = std::string(language) + " " + std::to_string(window.w) + "x" + std::to_string(window.h);
      NewContext(window);
      PeoplePickerController picker;
      MessagingContactsPorts contacts;
      contacts.snapshot = []() {
        MessagingView view;
        view.initialized = true;
        view.messaging_ready = false;
        return view;
      };
      picker.BindContactsPorts(contacts);
      PaneSpec opened;
      ShellNavigationPorts navigation;
      navigation.push_layer = [&opened](const PaneSpec& spec) {
        opened = spec;
        return 7;
      };
      navigation.close_layer = [](int) {};
      picker.BindShellNavigation(navigation);
      ASSERT_TRUE(picker.RegisterModel(context_)) << where;
      PeoplePickerChooseOptions options;
      options.title = LocalizationService::Instance().Tr("chat.forward.pick_title").c_str();
      options.send_label = LocalizationService::Instance().Tr("chat.forward.send").c_str();
      options.preview =
          "A long message that is being forwarded to several chats at once, over two lines of the strip "
          "and then some more words so that it has to be cut short";
      options.limit_message = "limit";
      options.list = []() { return Targets(); };
      picker.OpenForResult(std::move(options), [](std::vector<ForwardTarget>) {});
      EXPECT_EQ(opened.key, "people_picker") << where;
      EXPECT_TRUE(opened.modal) << "the desktop shows it as a modal card: " << where;
      EXPECT_EQ(opened.rml_path, "people_picker_wide") << where;

      ShellState state;
      state.layout_mode = LayoutMode::Expanded;
      state.nav_tab = NavTab::Sessions;
      state.desktop_computer = true;
      state.panes.push_back(
          {.spec = {.key = "chat", .rml_path = "views/chat.rml", .provides_composer = true}, .id = 1});
      state.primary_pane_key = "chat";
      OverlayEntry layer;
      layer.id = 7;
      layer.key = opened.key;
      layer.rml_path = opened.rml_path;
      layer.page_sheet = opened.page_sheet;
      layer.modal = opened.modal;
      state.overlay_stack.push_back(layer);
      LoadShell(window, state,
                [](std::string& root) { Fill(root, "overlay-body-7", Asset("views/people_picker_wide.rml")); });

      ui::Element* frame = First("shell-frame--people_picker");
      ASSERT_NE(frame, nullptr) << where;
      const Box card = BoxOf(frame);
      EXPECT_GE(card.left, kMargin) << where;
      EXPECT_GE(card.top, kMargin) << where;
      EXPECT_LE(card.right, static_cast<float>(window.w) - kMargin) << where;
      EXPECT_LE(card.bottom, static_cast<float>(window.h) - kMargin) << where;
      EXPECT_LE(card.W(), 480.5f) << where;
      EXPECT_GE(card.W(), 400.f) << where;
      EXPECT_NEAR((card.left + card.right) / 2, window.w / 2.f, 1.5f) << "centred: " << where;

      ui::Element* picker_root = First("people-picker--wide");
      ASSERT_NE(picker_root, nullptr) << where;
      for (const char* cls : {"people-picker-title", "people-picker-search-field", "people-picker-preview",
                              "people-picker-wide-cancel", "people-picker-wide-send"}) {
        ui::Element* element = First(cls);
        ASSERT_NE(element, nullptr) << cls << " " << where;
        ASSERT_TRUE(Drawn(element)) << cls << " " << where;
        const Box b = BoxOf(element);
        EXPECT_GE(b.left, card.left + kMargin) << cls << " " << where;
        EXPECT_LE(b.right, card.right - kMargin) << cls << " " << where;
        EXPECT_GE(b.top, card.top + kMargin) << cls << " " << where;
        EXPECT_LE(b.bottom, card.bottom - kMargin) << cls << " " << where;
      }
      EXPECT_LE(BoxOf(First("people-picker-wide-cancel")).right, BoxOf(First("people-picker-wide-send")).left) << where;
      // The rows sit in the scrolling list, which sits between the strip and the buttons; each row is inside the card.
      ui::Element* list = First("people-picker-wide-list");
      ASSERT_NE(list, nullptr) << where;
      const Box list_box = BoxOf(list);
      EXPECT_GT(list_box.H(), 90.f) << "the list keeps room at the smallest window: " << where;
      EXPECT_LE(list_box.bottom, BoxOf(First("people-picker-wide-send")).top) << where;
      EXPECT_GE(list_box.top, BoxOf(First("people-picker-preview")).bottom - 0.5f) << where;
      ui::ElementList rows; // the data-for keeps its template element, not shown
      for (ui::Element* row : All("people-picker-row")) {
        if (Drawn(row)) {
          rows.push_back(row);
        }
      }
      ASSERT_GE(rows.size(), 12u) << where;
      size_t inside = 0;
      for (ui::Element* row : rows) {
        const Box b = BoxOf(row);
        EXPECT_GE(b.left, card.left + kMargin) << where;
        EXPECT_LE(b.right, card.right - kMargin) << where;
        if (b.top >= list_box.top - 0.5f && b.bottom <= list_box.bottom + 0.5f) {
          ++inside;
        }
      }
      EXPECT_GE(inside, 2u) << "some rows show, the rest scroll: " << where;
      // A long name stays on its row (one line, cut at the row's width), never over the check or the edge.
      for (ui::Element* title : All("people-picker-row-title")) {
        const Box t = BoxOf(title);
        EXPECT_LE(t.right, card.right - kMargin) << where;
      }
    }
  }
}

} // namespace
} // namespace pbr
