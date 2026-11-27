// Forwarding on the desktop (DEV/PLAN-message-forward-text.md, step 5): choosing messages in the wide chat (the top bar
// "N selected / Cancel" and the bottom Forward bar stay inside the conversation column) and the "Forward to" card
// (people_picker_wide.rml) staying inside its window. The real views and stylesheets, the real shell markup; the
// catalog is loaded in each language directly, not through the app's language choice.
#include "feature/presenters/ChatPresenter.h"
#include "feature/presenters/MessageForward.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/FlowCoordinator.h"
#include "gui/chat/ChatController.h"
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/ForwardProgressController.h"
#include "gui/chat/TranscriptRml.h"
#include "gui/contacts/PeoplePickerController.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/ui/ViewCatalog.h"
#include "gui/shell/DataModelHost.h"
#include "gui/shell/ShellLayoutRml.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <map>
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

// The Forward button is a normal-sized button (not a strip across the column), and a click anywhere on the bar goes to
// the bar: the rows' covers (z-index 7) and ticks (z-index 8) used to sit above the floating composer strip (z-index 2),
// so a click on Forward also ticked / unticked the row under it. The newest row also rests above the strip.
TEST_F(ForwardWideTest, WideForwardBarTakesItsOwnClicksAndKeepsTheLastRowClear) {
  for (const Window& window : kWindows) {
    const std::string where = std::to_string(window.w) + "x" + std::to_string(window.h);
    NewContext(window);
    ChatScreenState chat;
    chat.use_messages_layout = true;
    chat.has_turns = true;
    chat.thread_title = "Ann Lee";
    chat.forward_selecting = true;
    chat.forward_can_next = true;
    chat.forward_count_label = "9 selected";
    std::vector<std::string> ids;
    for (int i = 0; i < 30; ++i) {
      ids.push_back("m" + std::to_string(i));
    }
    for (int i = 0; i < 30; ++i) {
      chat.messages.push_back(ChooseRow(ids[i], i % 2 == 0, true, i % 3 == 0));
    }
    int taps = 0;
    int nexts = 0;
    ASSERT_TRUE(DataModelHost::Instance().Register(context_, "chat", [&](ui::DataModelConstructor& ctor) {
      BindChatScreenState(ctor, chat);
      ctor.BindEventCallback("forward_tap",
                             [&taps](ui::DataModelHandle, ui::Event&, const ui::VariantList&) { ++taps; });
      ctor.BindEventCallback("forward_next",
                             [&nexts](ui::DataModelHandle, ui::Event&, const ui::VariantList&) { ++nexts; });
      ctor.BindEventCallback("messages_scroll", [](ui::DataModelHandle, ui::Event&, const ui::VariantList&) {});
    }));
    ShellState state;
    state.layout_mode = LayoutMode::Expanded;
    state.nav_tab = NavTab::Sessions;
    state.panes.push_back({.spec = {.key = "chat", .rml_path = "views/chat.rml", .provides_composer = true}, .id = 1});
    state.primary_pane_key = "chat";
    LoadShell(window, state, [](std::string& root) {
      Fill(root, "pane-body-chat", Asset("views/chat_wide.rml"));
      Fill(root, "pane-composer-chat", Asset("views/composer_wide.rml"));
    });
    ui::Element* messages = document_->QuerySelector("#chat-messages");
    ASSERT_NE(messages, nullptr) << where;
    for (int i = 0; i < 4; ++i) {
      ChatController::KeepClearOfComposer(document_->QuerySelector("#pane-chat"), "pane-composer-chat");
      context_->Update();
      messages->SetScrollTop(messages->GetScrollHeight());
      context_->Update();
    }
    // The newest row (and its tick) rests above the floating strip's panel.
    ui::Element* panel = document_->QuerySelector(".shell-pane-composer-bar > .bar-material");
    ASSERT_NE(panel, nullptr) << where;
    ui::ElementList rows;
    messages->GetElementsByClassName(rows, "message-row");
    ASSERT_FALSE(rows.empty()) << where;
    EXPECT_LE(BoxOf(rows.back()).bottom, BoxOf(panel).top + 0.5f) << "the last row is not under the bar: " << where;
    for (ui::Element* tick : All("message-forward-check")) {
      if (Drawn(tick) && BoxOf(tick).top >= BoxOf(rows.back()).top) {
        EXPECT_LE(BoxOf(tick).bottom, BoxOf(panel).top + 0.5f) << where;
      }
    }
    // Scrolled up, so that rows lie under the floating strip.
    messages->SetScrollTop(messages->GetScrollHeight() / 2.f);
    for (int i = 0; i < 3; ++i) {
      context_->Update();
    }
    ui::Element* next = First("chat-forward-next");
    ASSERT_TRUE(next && Drawn(next)) << where;
    const Box button = BoxOf(next);
    EXPECT_GE(button.W(), 140.f) << where;
    EXPECT_LE(button.W(), 180.5f) << "a normal-sized button, not a strip: " << where;
    EXPECT_GE(button.H(), 44.f) << where;
    const Box col = BoxOf(document_->QuerySelector("#pane-chat"));
    EXPECT_NEAR((button.left + button.right) / 2, (col.left + col.right) / 2, 1.5f)
        << "centred in the column: " << where;
    // Every point of the button and of the bar's strip takes the click itself, never a row's cover or tick.
    ui::Element* strip = First("shell-pane-composer-bar");
    ASSERT_NE(strip, nullptr) << where;
    const Box bar = BoxOf(strip);
    for (const Box& area : {button, bar}) {
      for (const float fx : {0.1f, 0.5f, 0.9f}) {
        for (const float fy : {0.1f, 0.5f, 0.9f}) {
          const ui::Vector2f at{area.left + area.W() * fx, area.top + area.H() * fy};
          ui::Element* hit = context_->GetElementAtPoint(at);
          ASSERT_NE(hit, nullptr) << where;
          for (ui::Element* e = hit; e; e = e->GetParentNode()) {
            EXPECT_FALSE(e->IsClassSet("message-forward-cover") || e->IsClassSet("message-forward-check"))
                << "a click on the bar reaches a message row: " << where << " at " << at.x << "," << at.y;
          }
        }
      }
    }
    {
      size_t under = 0;
      const float cx = (button.left + button.right) / 2, cy = (button.top + button.bottom) / 2;
      for (ui::Element* cover : All("message-forward-cover")) {
        const Box b = BoxOf(cover);
        under += (b.left <= cx && cx <= b.right && b.top <= cy && cy <= b.bottom) ? 1u : 0u;
      }
      EXPECT_GE(under, 1u) << "precondition: a row's cover lies under the button: " << where;
    }
    // A real click on the button (move, press, release) is one Forward and no tick change.
    context_->ProcessMouseMove(static_cast<int>((button.left + button.right) / 2),
                               static_cast<int>((button.top + button.bottom) / 2), 0);
    context_->ProcessMouseButtonDown(0, 0);
    context_->ProcessMouseButtonUp(0, 0);
    context_->Update();
    EXPECT_EQ(nexts, 1) << where;
    EXPECT_EQ(taps, 0) << "the click also reached a row: " << where;
    // A click on a row above the bar still ticks it (the fix keeps the covers working).
    for (ui::Element* cover : All("message-forward-cover")) {
      const Box b = BoxOf(cover);
      if (b.top > col.top + 120.f && b.bottom < bar.top - 4.f) {
        context_->ProcessMouseMove(static_cast<int>((b.left + b.right) / 2), static_cast<int>((b.top + b.bottom) / 2),
                                   0);
        context_->ProcessMouseButtonDown(0, 0);
        context_->ProcessMouseButtonUp(0, 0);
        context_->Update();
        EXPECT_EQ(taps, 1) << "a click on a row above the bar ticks it: " << where;
        break;
      }
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

std::string ReadSource(const std::string& relative) {
  std::ifstream in(std::string(PP_SOURCE_DIR) + "/" + relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

ThreadMessage AttachmentRow(const std::string& id, const std::string& name, const std::string& mime,
                            const uint64_t bytes) {
  ChatAttachmentFields fields;
  fields.mime = mime;
  fields.filename = name;
  fields.byte_length = bytes;
  fields.url = "https://example.invalid/blob/" + id;
  fields.content_hash.assign(32, 1);
  fields.content_key.assign(32, 2);
  fields.blob_nonce.assign(24, 3);
  ThreadMessage row;
  row.id = id;
  row.thread_id = "t";
  row.sender_contact_id = "account:b";
  row.content_type = ChatContentType::Attachment;
  row.text = name;
  row.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
  row.delivery = MessageDelivery::Relayed;
  return row;
}

/** A picture, a file and a voice message drawn by the real transcript, the way a chat shows them. */
std::vector<MessageDisplayRow> AttachmentTranscript() {
  std::vector<TranscriptRow> entries;
  int64_t at = 1;
  for (const ThreadMessage& message : {AttachmentRow("a1", "photo.jpg", "image/jpeg", 900'000),
                                       AttachmentRow("a2", "report.pdf", "application/pdf", 120'000),
                                       AttachmentRow("a3", "voice-4200.ogg", "audio/ogg", 25'000)}) {
    TranscriptRow entry;
    entry.message = message;
    entry.message.timestamp = 1700049600000 + at++ * 60'000;
    entries.push_back(entry);
  }
  TranscriptFacts facts;
  facts.sender_label = [](const std::string& id) { return id; };
  facts.sender_stable_id = [](const std::string& id) { return id; };
  facts.voice_supported = true;
  facts.voice_max_seconds = 120;
  facts.attachment = [](const ThreadMessage&) {
    TranscriptAttachmentView view;
    view.state = AttachmentFetchWorkflow::DownloadState::Ready;
    view.local_path = "/views/preview.jpg";
    view.inline_image_ok = true;
    view.image_width = 800;
    view.image_height = 600;
    return view;
  };
  return RenderTranscript(entries, facts);
}

// Choosing messages in the wide chat: a picture, a file and a voice message (drawn by the real transcript) each show
// their tick box and the cover that takes a click; no hover bar is drawn while choosing.
TEST_F(ForwardWideTest, WideAttachmentRowsHaveCheckboxesInSelectionMode) {
  for (const Window& window : kWindows) {
    const std::string where = std::to_string(window.w) + "x" + std::to_string(window.h);
    NewContext(window);
    ChatScreenState chat;
    chat.use_messages_layout = true;
    chat.has_turns = true;
    chat.thread_title = "Ann Lee";
    chat.forward_selecting = true;
    chat.forward_can_next = true;
    chat.forward_count_label = LocalizationService::Instance().Tr("chat.forward.selected_count", {{"n", "1"}}).c_str();
    chat.messages = AttachmentTranscript();
    ASSERT_EQ(chat.messages.size(), 3u) << where;
    for (MessageDisplayRow& row : chat.messages) {
      EXPECT_TRUE(row.forwardable) << "a picture, a file and a voice message can be chosen: " << where;
      EXPECT_TRUE(row.bar_forward) << "and the hover bar offers Forward on them: " << where;
      row.forward_mode = true;
      row.forward_ok = row.forwardable;
    }
    chat.messages[1].forward_ticked = true;
    ASSERT_TRUE(DataModelHost::Instance().Register(
        context_, "chat", [&chat](ui::DataModelConstructor& ctor) { BindChatScreenState(ctor, chat); }));
    ShellState state;
    state.layout_mode = LayoutMode::Expanded;
    state.nav_tab = NavTab::Sessions;
    state.panes.push_back({.spec = {.key = "chat", .rml_path = "views/chat.rml", .provides_composer = true}, .id = 1});
    state.primary_pane_key = "chat";
    LoadShell(window, state, [](std::string& root) {
      Fill(root, "pane-body-chat", Asset("views/chat_wide.rml"));
      Fill(root, "pane-composer-chat", Asset("views/composer_wide.rml"));
    });
    const Box col = BoxOf(document_->QuerySelector("#pane-chat"));
    size_t checks = 0;
    size_t ticked = 0;
    for (ui::Element* tick : All("message-forward-check")) {
      if (!Drawn(tick)) {
        continue;
      }
      ++checks;
      const Box b = BoxOf(tick);
      EXPECT_GE(b.left, col.left) << where;
      EXPECT_LE(b.right, col.right) << where;
    }
    for (ui::Element* tick : All("message-forward-check--on")) {
      ticked += Drawn(tick) ? 1u : 0u;
    }
    EXPECT_EQ(checks, 3u) << "every attachment row shows its tick box: " << where;
    EXPECT_EQ(ticked, 1u) << where;
    size_t covers = 0;
    for (ui::Element* cover : All("message-forward-cover")) {
      covers += Drawn(cover) ? 1u : 0u;
    }
    EXPECT_EQ(covers, 3u) << where;
    for (ui::Element* bar : All("msg-bar")) {
      EXPECT_FALSE(Drawn(bar)) << "no hover bar while choosing: " << where;
    }
  }
}

// The desktop's menus (the right-click menu, the hover bar's "..." and its buttons: one list, ChatMenus::ForRequest)
// have no Copy on a picture, a video, a file or a voice message, and have Forward and Select; Copy stays on words.
TEST_F(ForwardWideTest, TheDesktopMenusOfferNoCopyOnAnAttachment) {
  const std::string menus = ReadSource("src/gui/chat/ChatMenus.cpp");
  const size_t for_request = menus.find("std::vector<ContextMenuAction> ChatMenus::ForRequest(");
  ASSERT_NE(for_request, std::string::npos);
  const size_t copy = menus.find("\"copy_message\"", for_request);
  ASSERT_NE(copy, std::string::npos);
  const size_t guard = menus.rfind("if (", copy);
  EXPECT_EQ(menus.substr(guard, menus.find('{', guard) - guard), "if (!presenter_.IsAttachmentMessage(message_id)) ")
      << "Copy is made only for a message that is not an attachment, in the one list every desktop menu shows";
  EXPECT_EQ(menus.find("\"copy_message\"", copy + 1), std::string::npos) << "the only place the item is made";
  EXPECT_NE(menus.find("\"forward_message\"", for_request), std::string::npos);
  EXPECT_NE(menus.find("\"select_messages\"", for_request), std::string::npos);
  // The hover bar has no Copy button of its own and its "..." opens that list.
  const std::string page = Asset("views/chat_wide.rml");
  const size_t bar = page.find("class=\"msg-bar\"");
  ASSERT_NE(bar, std::string::npos);
  const std::string bar_markup = page.substr(bar, page.find("class=\"message-group\"", bar) - bar);
  EXPECT_EQ(bar_markup.find("copy"), std::string::npos);
  EXPECT_NE(bar_markup.find("msg_bar_more()"), std::string::npos);
  EXPECT_NE(bar_markup.find("msg_bar_forward()"), std::string::npos);
  // What counts as an attachment message: each kind, and not words or a missing row.
  ChatPresenter presenter;
  std::map<std::string, ThreadMessage> rows;
  for (const ThreadMessage& row : {AttachmentRow("photo.jpg", "photo.jpg", "image/jpeg", 2'000'000),
                                   AttachmentRow("clip.mov", "clip.mov", "video/quicktime", 9'000'000),
                                   AttachmentRow("report.pdf", "report.pdf", "application/pdf", 90'000),
                                   AttachmentRow("voice-4200.ogg", "voice-4200.ogg", "audio/ogg", 25'000)}) {
    rows[row.id] = row;
  }
  ThreadMessage words;
  words.id = "words";
  words.thread_id = "t";
  words.sender_contact_id = "account:b";
  words.text = "words";
  rows["words"] = words;
  ForwardPorts ports;
  ports.active_thread = []() { return std::string("t"); };
  ports.get_message = [&rows](const std::string&, const std::string& id) -> std::optional<ThreadMessage> {
    const auto it = rows.find(id);
    return it == rows.end() ? std::nullopt : std::optional<ThreadMessage>(it->second);
  };
  presenter.SetForwardPortsForTest(ports);
  for (const char* id : {"photo.jpg", "clip.mov", "report.pdf", "voice-4200.ogg"}) {
    EXPECT_TRUE(presenter.IsAttachmentMessage(id)) << id;
  }
  EXPECT_FALSE(presenter.IsAttachmentMessage("words"));
  EXPECT_FALSE(presenter.IsAttachmentMessage("gone"));
}

// The "Forward to" card with attachments: the same three tiles as the phone (a picture's thumbnail, a video's icon, a
// file's name and size, a voice message's length; grey where this device has nothing), "N messages", the note, and the
// reason Send is off above the strip, with Send greyed; everything inside the card with room to spare, English and Chinese.
TEST_F(ForwardWideTest, PeoplePickerWideShowsThePreviewTilesAndTheRefusalLine) {
  const std::string markup = Asset("views/people_picker_wide.rml");
  for (const char* part :
       {"preview_items", "item.thumb", "item.label", "item.grey", "preview_note", "choose_refusal"}) {
    EXPECT_NE(markup.find(part), std::string::npos) << part;
  }
  EXPECT_LT(markup.find("choose_refusal"), markup.find("preview_items")) << "the reason is above the strip";
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
      LocalizationService& tr = LocalizationService::Instance();
      PeoplePickerChooseOptions options;
      options.title = tr.Tr("chat.forward.pick_title").c_str();
      options.send_label = tr.Tr("chat.forward.send").c_str();
      options.preview = tr.Tr("chat.forward.preview_count", {{"n", "5"}});
      options.preview_items = {
          {"photo", "/scratch/0.jpg", false, ""},
          {"file", "", false, "Quarterly-report-final-version-2026.pdf · 3 MB"},
          {"mic", "", true, tr.Tr("chat.forward.preview_voice", {{"t", "0:42"}})},
      };
      options.preview_note =
          "2 aren't on this device and won't be forwarded; the long second half of the line makes it wrap";
      options.refusal = [](size_t) {
        return std::string("Files and pictures go 5 at a time (now 6): pick fewer messages or fewer chats");
      };
      options.limit_message = "limit";
      options.list = []() { return Targets(); };
      picker.OpenForResult(std::move(options), [](std::vector<ForwardTarget>) {});
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
      EXPECT_GE(card.top, kMargin) << where;
      EXPECT_LE(card.bottom, static_cast<float>(window.h) - kMargin) << where;

      ui::Element* refusal = First("people-picker-refusal");
      ui::Element* strip = First("people-picker-preview");
      ui::Element* note = First("people-picker-preview-note");
      ui::Element* list = First("people-picker-wide-list");
      ui::Element* send = First("people-picker-wide-send");
      ASSERT_TRUE(refusal && strip && note && list && send) << where;
      for (ui::Element* element : {refusal, strip, note, send}) {
        ASSERT_TRUE(Drawn(element)) << where;
        const Box b = BoxOf(element);
        EXPECT_GE(b.left, card.left + kMargin) << where;
        EXPECT_LE(b.right, card.right - kMargin) << where;
        EXPECT_GE(b.top, card.top + kMargin) << where;
        EXPECT_LE(b.bottom, card.bottom - kMargin) << where;
      }
      EXPECT_LE(BoxOf(refusal).bottom, BoxOf(strip).top + 0.5f) << "the reason is above the strip: " << where;
      EXPECT_LE(BoxOf(strip).bottom, BoxOf(list).top + 0.5f) << where;
      EXPECT_LE(BoxOf(note).bottom, BoxOf(strip).bottom + 0.5f) << "the note is inside the strip: " << where;
      EXPECT_LE(BoxOf(list).bottom, BoxOf(send).top) << where;
      EXPECT_GT(BoxOf(list).H(), 90.f) << "the rows keep room at the smallest window: " << where;
      // Send is greyed while the reason shows.
      EXPECT_TRUE(send->HasAttribute("disabled")) << where;

      // Three tiles, in order, each inside the strip; the picture has its thumbnail, the file its name, the voice
      // message is dimmed (nothing of it on this device).
      std::vector<ui::Element*> tiles;
      for (ui::Element* item : All("people-picker-preview-item")) {
        if (Drawn(item)) {
          tiles.push_back(item);
        }
      }
      ASSERT_EQ(tiles.size(), 3u) << where;
      for (ui::Element* tile : tiles) {
        const Box b = BoxOf(tile);
        EXPECT_GE(b.left, BoxOf(strip).left) << where;
        EXPECT_LE(b.right, BoxOf(strip).right) << where;
        EXPECT_GE(b.top, BoxOf(strip).top) << where;
        EXPECT_LE(b.bottom, BoxOf(strip).bottom + 0.5f) << where;
      }
      EXPECT_LT(BoxOf(tiles[0]).left, BoxOf(tiles[1]).left) << where;
      EXPECT_LT(BoxOf(tiles[1]).left, BoxOf(tiles[2]).left) << where;
      const auto shown_in = [](ui::Element* tile, const char* cls) {
        ui::ElementList found;
        tile->GetElementsByClassName(found, cls);
        for (ui::Element* element : found) {
          if (Drawn(element)) {
            return true;
          }
        }
        return false;
      };
      EXPECT_TRUE(shown_in(tiles[0], "people-picker-preview-thumb")) << "the picture shows its file: " << where;
      EXPECT_FALSE(shown_in(tiles[1], "people-picker-preview-thumb")) << where;
      EXPECT_TRUE(shown_in(tiles[1], "people-picker-preview-label")) << "the file's name and size: " << where;
      EXPECT_TRUE(shown_in(tiles[1], "people-picker-preview-icon")) << "its type icon: " << where;
      EXPECT_TRUE(tiles[2]->IsClassSet("people-picker-preview-item--grey")) << "no bytes here: " << where;
      EXPECT_FALSE(tiles[0]->IsClassSet("people-picker-preview-item--grey")) << where;
      EXPECT_LT(tiles[2]->GetComputedValues().opacity(), 1.f) << where;
      for (const char* cls : {"people-picker-wide-cancel", "people-picker-wide-send"}) {
        EXPECT_TRUE(Drawn(First(cls))) << cls << " " << where;
      }
    }
  }
}

// The "Forwarding 2/7" card in the window: a centred card, every control inside it, in both languages, at the three
// window sizes and in each state the page has (a percent bar; a spinner with the elapsed seconds; the rate-limit wait;
// finishing; Close after 15 s).
TEST_F(ForwardWideTest, ForwardProgressWideKeepsEverythingInsideTheCard) {
  EXPECT_EQ(ViewCatalog::KeyForLayout("forward_progress", true), "forward_progress_wide")
      << "the wide window mounts the card; the phone page is the compact layout's";
  EXPECT_EQ(ViewCatalog::KeyForLayout("forward_progress", false), "forward_progress");
  struct Page {
    const char* name;
    ForwardProgressView view;
  };
  const std::vector<Page> pages = {
      {"percent", {"Forwarding 12/30", "", 37, false, false}},
      {"spinner", {"Forwarding 3/5", "Sending… 21s", -1, false, false}},
      {"wait", {"Forwarding 3/5", "One moment, almost there", -1, false, false}},
      {"finishing", {"Forwarding 3/5", "Finishing — the current item will still go through…", -1, true, false}},
      {"close", {"Forwarding 3/5", "Finishing — the current item will still go through…", -1, true, true}},
  };
  for (const char* language : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(language);
    LocalizationService& tr = LocalizationService::Instance();
    for (const Window& window : std::vector<Window>{{1280, 800}, {1000, 700}, {1280, 720}, {1024, 640}, {900, 600}}) {
      Box ring_box{};
      float ring_frame_height = 0.f;
      for (const Page& page : pages) {
        const std::string where =
            std::string(language) + " " + std::to_string(window.w) + "x" + std::to_string(window.h) + " " + page.name;
        NewContext(window);
        ForwardProgressController progress;
        ASSERT_TRUE(progress.RegisterModel(context_)) << where;
        ForwardProgressView view = page.view;
        if (std::string(language) == "zh-Hans") {
          view.title = tr.Tr("chat.forward.progress_title", {{"done", "12"}, {"total", "30"}});
          if (std::string(page.name) == "spinner") {
            view.line = tr.Tr("chat.forward.progress_sending", {{"s", "21"}});
          } else if (std::string(page.name) == "wait") {
            view.line = tr.Tr("chat.forward.progress_wait");
          } else if (view.finishing) {
            view.line = tr.Tr("chat.forward.progress_finishing");
          }
        }
        progress.Show(view);
        ShellState state;
        state.layout_mode = LayoutMode::Expanded;
        state.nav_tab = NavTab::Sessions;
        state.desktop_computer = true;
        state.panes.push_back(
            {.spec = {.key = "chat", .rml_path = "views/chat.rml", .provides_composer = true}, .id = 1});
        state.primary_pane_key = "chat";
        OverlayEntry layer;
        layer.id = 7;
        layer.key = "forward_progress";
        layer.rml_path = "forward_progress";
        layer.full_screen = true;
        layer.modal = true;
        state.overlay_stack.push_back(layer);
        LoadShell(window, state,
                  [](std::string& root) { Fill(root, "overlay-body-7", Asset("views/forward_progress_wide.rml")); });

        ui::Element* card_element = First("forward-progress-card");
        ASSERT_NE(card_element, nullptr) << where;
        ASSERT_TRUE(Drawn(card_element)) << where;
        const Box card = BoxOf(card_element);
        EXPECT_GE(card.left, kMargin) << where;
        EXPECT_GE(card.top, kMargin) << where;
        EXPECT_LE(card.right, static_cast<float>(window.w) - kMargin) << where;
        EXPECT_LE(card.bottom, static_cast<float>(window.h) - kMargin) << where;
        EXPECT_NEAR((card.left + card.right) / 2, window.w / 2.f, 1.5f) << "centred: " << where;
        EXPECT_LE(card.W(), 380.5f) << "a small card, not a page: " << where;
        // The frame is the card the person sees: inside the window, centred, small, with its close button in its corner,
        // over a dimmed scrim that covers the window and takes the clicks.
        ui::Element* frame_element = First("shell-frame--forward_progress");
        ASSERT_NE(frame_element, nullptr) << where;
        const Box frame = BoxOf(frame_element);
        EXPECT_LE(frame.W(), 380.5f) << where;
        EXPECT_LE(frame.H(), 420.f) << "content height, not the window: " << where;
        EXPECT_GE(frame.left, kMargin) << where;
        EXPECT_LE(frame.right, static_cast<float>(window.w) - kMargin) << where;
        EXPECT_NEAR((frame.left + frame.right) / 2, window.w / 2.f, 1.5f) << where;
        EXPECT_NEAR((frame.top + frame.bottom) / 2, window.h / 2.f, 20.f) << where;
        EXPECT_GE(card.left, frame.left - 0.5f) << where;
        EXPECT_LE(card.right, frame.right + 0.5f) << where;
        ui::Element* close_x = First("shell-close-btn");
        ASSERT_TRUE(close_x && Drawn(close_x)) << where;
        const Box x = BoxOf(close_x);
        EXPECT_GE(x.left, frame.left) << where;
        EXPECT_LE(x.right, frame.right) << where;
        EXPECT_GE(x.top, frame.top) << where;
        EXPECT_LE(x.bottom, frame.top + 56.f) << "the close button is in the card's top corner: " << where;
        ui::Element* scrim = document_->GetElementById("shell-modal-scrim-7");
        ASSERT_NE(scrim, nullptr) << where;
        EXPECT_GE(BoxOf(scrim).W(), static_cast<float>(window.w) - 0.5f) << "the scrim covers the window: " << where;
        EXPECT_GE(BoxOf(scrim).H(), static_cast<float>(window.h) - 0.5f) << where;
        // The scrim is translucent (the theme's #RRGGBBAA), so the chat shows through; the stage over it takes the clicks
        // (a click beside the card must not close the layer, which would stop the forward).
        EXPECT_TRUE(std::regex_search(Asset("themes/colors-light.rcss"),
                                      std::regex("\\.shell-scrim \\{\\s*background-color: #[0-9a-f]{6}[0-8][0-9a-f];")))
            << where;
        ui::Element* stage = document_->GetElementById("shell-modal-stage-7");
        ASSERT_NE(stage, nullptr) << where;
        EXPECT_TRUE(stage->IsClassSet("shell-modal-stage--forward_progress")) << where;
        EXPECT_EQ(stage->GetComputedValues().pointer_events(), ui::Style::PointerEvents::Auto) << where;
        // Both "title / status / buttons" are in the frame and visible (below).

        std::vector<const char*> parts = {"forward-progress-title", "forward-progress-activity"};
        if (!view.line.empty()) {
          parts.push_back("forward-progress-line"); // a figure's page has none: it is in the ring
        }
        if (!view.finishing) {
          parts.push_back("forward-progress-stay");
        }
        for (const char* cls : parts) {
          ui::Element* element = First(cls);
          ASSERT_NE(element, nullptr) << cls << " " << where;
          ASSERT_TRUE(Drawn(element)) << cls << " " << where;
          const Box b = BoxOf(element);
          EXPECT_GE(b.left, card.left + kMargin) << cls << " " << where;
          EXPECT_LE(b.right, card.right - kMargin) << cls << " " << where;
          EXPECT_GE(b.top, card.top + kMargin) << cls << " " << where;
          EXPECT_LE(b.bottom, card.bottom - kMargin) << cls << " " << where;
        }
        for (ui::Element* button : All("forward-progress-btn")) {
          if (!Drawn(button)) {
            continue;
          }
          const Box b = BoxOf(button);
          EXPECT_GE(b.left, card.left + kMargin) << where;
          EXPECT_LE(b.right, card.right - kMargin) << where;
          EXPECT_LE(b.bottom, card.bottom - kMargin) << where;
          EXPECT_GE(b.H(), 44.f) << where;
        }
        size_t buttons = 0;
        for (ui::Element* button : All("forward-progress-btn")) {
          buttons += Drawn(button) ? 1u : 0u;
        }
        EXPECT_EQ(buttons, view.finishing ? (view.can_close ? 1u : 0u) : (view.can_close ? 2u : 1u)) << where;
        // The line under the title in a window's words, not the phone's.
        const std::string words = card_element->GetInnerRML().c_str();
        EXPECT_EQ(words.find(tr.Tr("chat.forward.progress_stay").c_str()), std::string::npos) << where;
        // The ring (with the percentage in it) or the spinner, one of them, inside the card; never a bar.
        ui::Element* ring = First("forward-progress-ring");
        ui::Element* spinner = First("forward-progress-spinner");
        const bool ring_drawn = Drawn(ring);
        const bool spinner_drawn = Drawn(spinner);
        EXPECT_NE(ring_drawn, spinner_drawn) << where;
        EXPECT_EQ(ring_drawn, view.percent >= 0) << where;
        EXPECT_EQ(First("forward-progress-bar"), nullptr) << "the bar is gone: " << where;
        ui::Element* shown = ring_drawn ? ring : spinner;
        const Box shown_box = BoxOf(shown);
        EXPECT_NEAR(shown_box.W(), 72.f, 1.f) << where;
        EXPECT_NEAR(shown_box.H(), 72.f, 1.f) << where;
        EXPECT_GE(shown_box.left, card.left + kMargin) << where;
        EXPECT_LE(shown_box.right, card.right - kMargin) << where;
        EXPECT_NEAR((shown_box.left + shown_box.right) / 2, (card.left + card.right) / 2, 1.5f) << where;
        if (ring_drawn) {
          ui::Element* text = First("forward-progress-ring-text");
          ASSERT_NE(text, nullptr) << where;
          EXPECT_EQ(std::string(text->GetInnerRML().c_str()), "37%") << where;
          const Box core = BoxOf(text->GetParentNode());
          const Box t = BoxOf(text);
          EXPECT_GE(t.left, core.left - 0.5f) << "the figure is inside the ring: " << where;
          EXPECT_LE(t.right, core.right + 0.5f) << where;
          ring_box = shown_box;
          ring_frame_height = frame.H();
        } else if (ring_frame_height > 0.f && !view.finishing) {
          EXPECT_NEAR(shown_box.left, ring_box.left, 1.f) << "the spinner sits where the ring was: " << where;
          EXPECT_NEAR(shown_box.top, ring_box.top, 1.f) << where;
          EXPECT_NEAR(frame.H(), ring_frame_height, 1.f) << "the card does not change height: " << where;
        }
      }
    }
  }
}

// The wide progress card is frosted glass in both themes: a translucent surface (about 58%) over a backdrop blur on its
// rule in wide.rcss, a lighter scrim of its own, and the ring's disc and white figure stay readable on that surface.
TEST_F(ForwardWideTest, ForwardProgressCardSurfaceIsTranslucentInBothThemes) {
  const std::regex frame(
      "\\.shell-body--wide \\.shell-frame--forward_progress \\{\\s*background-color: #[0-9a-f]{6}([0-9a-f]{2});");
  const std::regex disc(
      "\\.forward-progress-ring,\\s*\\.forward-progress-spinner \\{\\s*background-color: "
      "#[0-9a-f]{6}([0-9a-f]{2});");
  for (const char* theme : {"themes/colors-light.rcss", "themes/colors-dark.rcss"}) {
    const std::string css = Asset(theme);
    std::smatch found;
    ASSERT_TRUE(std::regex_search(css, found, frame)) << theme;
    const int alpha = std::stoi(found[1].str(), nullptr, 16);
    EXPECT_LT(alpha, 255) << "the card surface is translucent: " << theme;
    EXPECT_GE(alpha, 0x80) << "but not so thin that the text behind wins: " << theme;
    EXPECT_LE(alpha, 0xa0) << "the glass lets the chat through (about 55-62%): " << theme;
    ASSERT_TRUE(std::regex_search(css, found, disc)) << theme;
    EXPECT_GE(std::stoi(found[1].str(), nullptr, 16), 0x60) << "the ring's disc stays visible: " << theme;
    EXPECT_EQ(css.find("forward-progress-bar"), std::string::npos) << "no bar rules left: " << theme;
    EXPECT_NE(css.find(".forward-progress-ring-text {\n  color: #ffffff;"), std::string::npos)
        << "a white figure on the dark core: " << theme;
  }
  const std::string wide = Asset("themes/wide.rcss");
  const std::regex blur(
      "\\.shell-body--wide \\.shell-modal-stage \\.shell-frame--forward_progress \\{\\s*backdrop-filter: "
      "blur\\(([0-9]+)px\\);");
  std::smatch blurred;
  ASSERT_TRUE(std::regex_search(wide, blurred, blur)) << "the card rule carries the backdrop blur";
  EXPECT_GE(std::stoi(blurred[1].str()), 20) << "at least the bars' blur";
  EXPECT_NE(wide.find(".shell-frame--forward_progress.shell-frame--solid {\n  backdrop-filter: none;"),
            std::string::npos)
      << "transparency off: no blur";
  // The scrim of this layer only is lighter than the shared one.
  const std::regex scrim("\\.shell-scrim\\.shell-scrim--forward_progress \\{\\s*background-color: #0{6}([0-9a-f]{2});");
  const std::regex shared("\\.shell-scrim \\{\\s*background-color: #0{6}([0-9a-f]{2});");
  for (const char* theme : {"themes/colors-light.rcss", "themes/colors-dark.rcss"}) {
    const std::string css = Asset(theme);
    std::smatch own;
    std::smatch base;
    ASSERT_TRUE(std::regex_search(css, own, scrim)) << theme;
    ASSERT_TRUE(std::regex_search(css, base, shared)) << theme;
    EXPECT_LT(std::stoi(own[1].str(), nullptr, 16), std::stoi(base[1].str(), nullptr, 16)) << theme;
  }
}

// Words and spelling of the desktop line, and that it is the one the wide page shows.
TEST_F(ForwardWideTest, TheWideProgressPageSaysToKeepPPOpen) {
  LocalizationService& tr = LocalizationService::Instance();
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(tr.Tr("chat.forward.progress_stay_desktop"), "Keep PP open until this finishes.");
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(tr.Tr("chat.forward.progress_stay_desktop"), "发完之前请不要退出 PP。");
  EXPECT_NE(Asset("views/forward_progress_wide.rml").find("chat.forward.progress_stay_desktop"), std::string::npos);
  EXPECT_EQ(Asset("views/forward_progress_wide.rml").find("chat.forward.progress_stay}"), std::string::npos);
}

// A computer never gets the phone's "really in the background": the flow's stop is registered only on a phone, and no
// other call of it exists, so a window losing focus or being minimised cannot end a forward (the presenter's own
// notices for those two are no-ops, and the call-started stop is its own event).
TEST_F(ForwardWideTest, OnDesktopLosingFocusOrMinimisingDoesNotStopTheForward) {
  const std::string controller = ReadSource("src/gui/chat/ChatController.cpp");
  const size_t listener = controller.find("forward_flow_->OnAppEnteredBackground()");
  ASSERT_NE(listener, std::string::npos);
  EXPECT_EQ(controller.find("forward_flow_->OnAppEnteredBackground()", listener + 1), std::string::npos)
      << "the only caller";
  const size_t guard = controller.rfind("if (", listener);
  EXPECT_EQ(controller.substr(guard, controller.find('{', guard) - guard), "if (Platform::IsMobile()) ")
      << "registered on a phone only";
  // The app-wide background listener (what a desktop's focus loss / minimise reaches) never touches the forward.
  const size_t pause = controller.find("void ChatController::OnApplicationPause()");
  ASSERT_NE(pause, std::string::npos);
  const std::string pause_body = controller.substr(pause, controller.find("\n}\n", pause) - pause);
  EXPECT_EQ(pause_body.find("forward"), std::string::npos) << pause_body;
  // The presenter's two desktop notices do nothing.
  const std::string presenter = ReadSource("src/feature/presenters/ChatPresenter.h");
  EXPECT_NE(presenter.find("void NoteAppLostFocus() {}"), std::string::npos);
  EXPECT_NE(presenter.find("void NoteAppMinimized() {}"), std::string::npos);
  // And the page itself does not listen for focus either.
  const std::string progress = ReadSource("src/gui/chat/ForwardProgressController.cpp");
  EXPECT_EQ(progress.find("focus"), std::string::npos);
  EXPECT_EQ(progress.find("Background"), std::string::npos);
}

// Esc on the wide page is the back key's path (ShellHost::HandleDismiss asks the flow coordinator): Cancel, taken, and
// the layer stays up until the item under way answers. On a computer the page is the wide card.
TEST_F(ForwardWideTest, EscOnTheWideProgressPageCancels) {
  FlowCoordinator coordinator;
  int cancels = 0;
  int closed_layers = 0;
  int pushed = 0;
  PaneSpec spec;
  ShellNavigationPorts navigation;
  navigation.push_layer = [&](const PaneSpec& pane) {
    spec = pane;
    ++pushed;
    return 3;
  };
  navigation.close_layer = [&](int) { ++closed_layers; };
  coordinator.BindShellNavigation(navigation);
  FlowCoordinatorPorts ports;
  ports.begin_modal = [&](int layer, std::function<bool()> back, std::function<void()> cancel) {
    coordinator.BeginModal(layer, std::move(back), std::move(cancel));
  };
  ports.end_modal = [&]() { coordinator.EndModal(); };
  ForwardProgressController controller;
  controller.BindShellNavigation(navigation);
  controller.BindFlowCoordinator(ports);
  ForwardProgressController::Handlers handlers;
  handlers.back = [&cancels]() {
    ++cancels;
    return true;
  };
  controller.SetHandlers(handlers);
  controller.Open();
  ASSERT_EQ(pushed, 1);
  EXPECT_EQ(spec.key, "forward_progress");
  EXPECT_TRUE(spec.full_screen);
  EXPECT_TRUE(spec.modal) << "a card over a dimmed scrim that takes the clicks (the phone ignores it)";
  EXPECT_EQ(ViewCatalog::KeyForLayout(spec.rml_path, true), "forward_progress_wide") << "a window gets the card";
  EXPECT_TRUE(coordinator.HandleDismiss()) << "Esc is taken";
  EXPECT_EQ(cancels, 1) << "as Cancel";
  EXPECT_EQ(closed_layers, 0) << "the page waits for the item under way";
  EXPECT_TRUE(controller.IsOpen());
}

} // namespace
} // namespace pbr
