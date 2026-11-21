// The chat page in the wide layout (design step 2): chat_wide.rml with the real stylesheets, in a conversation column
// at the widths the window gives it at 768 / 900 / 1280. Header buttons, search in place, the hover bar and the chats
// list row, measured; plus the hover bar offering exactly what the message menu offers.
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/ChatMenus.h"
#include "gui/shell/DataModelHost.h"
#include "foundation/i18n/LocalizationService.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <ui/style/ComputedValues.h>
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

std::string Asset(const char* relative) {
  return ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative);
}

constexpr int kRail = 64;
constexpr int kList = 320;

/** The conversation column's width at a window width: the list gives way first, down to 280 (wide.rcss). */
int ConversationWidth(const int window) {
  const int list = std::max(280, std::min(kList, window - kRail - 420));
  return window - kRail - list;
}

class ChatWideLayoutTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    ASSERT_TRUE(ui::LoadFontFace(Path("fonts/LatoLatin-Regular.ttf")));
    ASSERT_TRUE(ui::LoadFontFace(Path("fonts/Inter-Regular.ttf"), /*fallback_face=*/true));
    context = ui::CreateContext("chat-wide-test", {1280, 800});
    ASSERT_NE(context, nullptr);
    ASSERT_TRUE(DataModelHost::Instance().Register(
        context, "chat", [this](ui::DataModelConstructor& ctor) { BindChatScreenState(ctor, state); }));
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext("chat-wide-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }
  static std::string Path(const char* relative) { return std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative; }

  /** The wide chat page in a column `column_dp` wide inside a wide-root window. */
  void Load(const int column_dp) {
    const std::string page = LocalizationService::Instance().LocalizeText(Asset("views/chat_wide.rml"));
    const std::string rml =
        "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
        Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") +
        "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
        "<div class=\"shell-body shell-body--wide\" style=\"position: absolute; top: 0; left: 0; width: " +
        std::to_string(column_dp) +
        "dp; height: 700dp;\"><div class=\"shell-layer shell-layer-base\">"
        "<div id=\"col\" class=\"shell-pane shell-pane-primary\" style=\"width: " +
        std::to_string(column_dp) + "dp; height: 700dp;\"><div class=\"shell-pane-body\">" + page +
        "</div></div></div></div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    context->Update();
  }
  void ShowChat() {
    state.use_messages_layout = true;
    state.has_turns = true;
    state.thread_title = "Ann Lee";
    state.thread_subtitle = "Encrypted \xC2\xB7 On all your devices";
    state.show_call_actions = true;
    state.show_search = true;
    state.show_thread_menu = true;
    MessageDisplayRow mine;
    mine.message_id = "m1";
    mine.row_class = "message-row-user";
    mine.content_rml = "<div class=\"bubble bubble-user\"><p class=\"bubble-text\">Hello there</p></div>";
    mine.has_actions = true;
    MessageDisplayRow theirs = mine;
    theirs.message_id = "m2";
    theirs.row_class = "message-row-peer";
    theirs.content_rml = "<div class=\"bubble bubble-assistant\"><p class=\"bubble-text\">Hi</p></div>";
    MessageDisplayRow notice = theirs;
    notice.message_id = "m3";
    notice.has_actions = false;
    mine.can_edit = true; // own text the menu offers Edit for
    state.messages = {mine, theirs, notice};
  }
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
  ui::Element* First(const char* cls) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    return found.empty() ? nullptr : found[0];
  }
  ui::ElementList All(const char* cls) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    return found;
  }

  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  ChatScreenState state;
};

TEST_F(ChatWideLayoutTest, TheHeaderHasCallSearchAndMoreInsideItAtEveryWidth) {
  for (const int window : {768, 900, 1280}) {
    const int column = ConversationWidth(window);
    EXPECT_GE(column, 420) << window;
    ShowChat();
    Load(column);
    ui::Element* header = First("chat-header");
    ui::Element* call = First("chat-header-call-btn");
    ui::Element* search = First("chat-header-search-btn");
    ui::Element* more = First("chat-header-more-btn");
    ASSERT_NE(header, nullptr);
    ASSERT_NE(call, nullptr);
    ASSERT_NE(search, nullptr) << "the search button is in the wide header (" << window << ")";
    ASSERT_NE(more, nullptr);
    const Box h = BoxOf(header);
    EXPECT_FLOAT_EQ(h.H(), 64.f) << window;
    EXPECT_LE(h.right, static_cast<float>(column) + 0.5f) << "the header fits its column at " << window;
    for (ui::Element* button : {call, search, more}) {
      const Box b = BoxOf(button);
      EXPECT_FLOAT_EQ(b.W(), 36.f) << window;
      EXPECT_FLOAT_EQ(b.H(), 36.f) << window;
      EXPECT_GE(b.left, h.left);
      EXPECT_LE(b.right, h.right + 0.5f);
    }
    // In a row, call then search then more.
    EXPECT_LT(BoxOf(call).right, BoxOf(search).left + 0.5f);
    EXPECT_LT(BoxOf(search).right, BoxOf(more).left + 0.5f);
    // The avatar sits in the header, 40dp.
    ui::Element* avatar = First("chat-header-avatar");
    ASSERT_NE(avatar, nullptr);
    EXPECT_FLOAT_EQ(BoxOf(avatar).W(), 40.f);
    document->Close();
    context->Update();
  }
}

TEST_F(ChatWideLayoutTest, SearchOpensInPlaceInTheHeader) {
  ShowChat();
  state.search_open = true;
  Load(ConversationWidth(900));
  ui::Element* bar = First("chat-search-bar");
  ASSERT_NE(bar, nullptr) << "the search box takes the header's place";
  EXPECT_NE(bar->GetComputedValues().display(), ui::Style::Display::None);
  EXPECT_NE(document->GetElementById("search-input"), nullptr);
  ui::Element* actions = First("chat-header-actions");
  ASSERT_NE(actions, nullptr);
  EXPECT_EQ(actions->GetComputedValues().display(), ui::Style::Display::None) << "the buttons give way to the box";
  const Box header = BoxOf(First("chat-header"));
  const Box b = BoxOf(bar);
  EXPECT_GE(b.left, header.left);
  EXPECT_LE(b.right, header.right + 0.5f);
  EXPECT_NE(First("chat-search-step"), nullptr) << "the step arrows and Cancel are in the same row";
  EXPECT_NE(First("chat-search-cancel"), nullptr);
}

TEST_F(ChatWideLayoutTest, TheHoverBarShowsOnlyUnderThePointerOnAMessageThatHasActions) {
  ShowChat();
  Load(ConversationWidth(900));
  ui::ElementList all_rows;
  document->GetElementsByClassName(all_rows, "message-row");
  ui::ElementList rows; // the transcript's rows (the page has other .message-row boxes: queued question, turns)
  for (ui::Element* row : all_rows) {
    if (row->HasAttribute("message-id")) {
      rows.push_back(row);
    }
  }
  ASSERT_EQ(rows.size(), 3u);
  auto bar_of = [](ui::Element* row) -> ui::Element* {
    ui::ElementList found;
    row->GetElementsByClassName(found, "msg-bar");
    return found.empty() ? nullptr : found[0];
  };
  for (size_t i = 0; i < 3; ++i) {
    ui::Element* bar = bar_of(rows[i]);
    ASSERT_NE(bar, nullptr) << i;
    EXPECT_EQ(bar->GetComputedValues().display(), ui::Style::Display::None) << "hidden until hovered: " << i;
    ui::ElementList buttons;
    bar->GetElementsByClassName(buttons, "msg-bar-btn");
    EXPECT_EQ(buttons.size(), 4u) << "+ (the emoji picker), edit, reply, more";
    ui::ElementList edit;
    bar->GetElementsByClassName(edit, "msg-bar-edit");
    ASSERT_EQ(edit.size(), 1u) << i;
    EXPECT_EQ(edit[0]->GetComputedValues().display() == ui::Style::Display::None, i != 0)
        << "the pencil is there for our own text only, absent (not greyed) otherwise: " << i;
  }
  for (size_t i = 0; i < 3; ++i) {
    rows[i]->SetPseudoClass("hover", true);
    context->Update();
    ui::Element* bar = bar_of(rows[i]);
    if (i < 2) {
      EXPECT_NE(bar->GetComputedValues().display(), ui::Style::Display::None) << "shown under the pointer: " << i;
      EXPECT_GE(BoxOf(bar).H(), 28.f);
      EXPECT_LE(BoxOf(bar).right, BoxOf(rows[i]).right + 0.5f) << "the bar stays inside the row's width";
    } else {
      EXPECT_EQ(bar->GetComputedValues().display(), ui::Style::Display::None) << "a notice row has no bar";
    }
    rows[i]->SetPseudoClass("hover", false);
    context->Update();
  }
}

TEST_F(ChatWideLayoutTest, TheHoverBarOffersExactlyTheMessageMenusItems) {
  // The used reactions (one button repeated over bar_emojis), "+" (the full picker), Reply and More: the reactions and
  // Reply run the message menu's own items, More opens that whole menu.
  const std::string page = Asset("views/chat_wide.rml");
  const size_t bar = page.find("class=\"msg-bar\"");
  ASSERT_NE(bar, std::string::npos);
  const std::string bar_markup = page.substr(bar, page.find("class=\"chat-row-body\"", bar) - bar);
  EXPECT_NE(bar_markup.find("msg_bar_emoji(row.message_id, e.emoji)"), std::string::npos);
  EXPECT_NE(bar_markup.find("msg_bar_react()"), std::string::npos);
  EXPECT_NE(bar_markup.find("msg_bar_edit(row.message_id)"), std::string::npos);
  EXPECT_NE(bar_markup.find("msg_bar_reply()"), std::string::npos);
  EXPECT_NE(bar_markup.find("msg_bar_more()"), std::string::npos);
  // The order is reactions, +, Edit, (Forward later), Reply, More.
  EXPECT_LT(bar_markup.find("msg_bar_emoji"), bar_markup.find("msg_bar_react"));
  EXPECT_LT(bar_markup.find("msg_bar_react"), bar_markup.find("msg_bar_edit"));
  EXPECT_LT(bar_markup.find("msg_bar_edit"), bar_markup.find("TODO(forward)"));
  EXPECT_LT(bar_markup.find("TODO(forward)"), bar_markup.find("msg_bar_reply"));
  EXPECT_LT(bar_markup.find("msg_bar_reply"), bar_markup.find("msg_bar_more"));
  // No other action in the bar (no forward, no save...): five buttons, the first repeated per reaction.
  size_t buttons = 0;
  for (size_t at = bar_markup.find("<button"); at != std::string::npos; at = bar_markup.find("<button", at + 1)) {
    ++buttons;
  }
  EXPECT_EQ(buttons, 5u);
  EXPECT_STREQ(MessageBarActionId(MessageBarAction::Reply), "reply_message");
  EXPECT_STREQ(MessageBarActionId(MessageBarAction::React), "react_more");
  EXPECT_STREQ(MessageBarActionId(MessageBarAction::More), "");
  // The ids are items ChatMenus::ForRequest builds for a message, and the bar's reactions are its quick row.
  const std::string menus = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatMenus.cpp");
  const size_t for_request = menus.find("std::vector<ContextMenuAction> ChatMenus::ForRequest(");
  ASSERT_NE(for_request, std::string::npos);
  EXPECT_NE(menus.find("\"reply_message\"", for_request), std::string::npos);
  EXPECT_NE(menus.find("\"react_more\"", for_request), std::string::npos);
  EXPECT_NE(menus.find("QuickReactions(host_.RecentEmojis())", for_request), std::string::npos);
  // Edit: the menu offers it on host_.CanEditMessage (and not while editing); the bar's pencil asks the same of the controller.
  EXPECT_NE(menus.find("host_.CanEditMessage(message_id)", for_request), std::string::npos);
  const std::string controller = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp");
  const size_t can_edit_at = controller.find("facts.can_edit =");
  ASSERT_NE(can_edit_at, std::string::npos);
  const std::string can_edit_body = controller.substr(can_edit_at, 220);
  EXPECT_NE(can_edit_body.find("!chat_.editing_message"), std::string::npos);
  EXPECT_NE(can_edit_body.find("CanEditMessage(message_id)"), std::string::npos);
  EXPECT_NE(controller.find("kBarEmojiCount = 5"), std::string::npos) << "five reactions";
  EXPECT_EQ(menus.find("forward", for_request), std::string::npos) << "no forwarding";
}

TEST_F(ChatWideLayoutTest, TheHoverBarShowsTheUsedReactionsBeforeThePlus) {
  ShowChat();
  for (const char* emoji :
       {"\xF0\x9F\x91\x8D", "\xE2\x9D\xA4", "\xF0\x9F\x98\x82", "\xF0\x9F\x98\xAE", "\xF0\x9F\x98\xA2"}) {
    BarEmojiRow row;
    row.emoji = emoji;
    state.bar_emojis.push_back(row);
  }
  Load(ConversationWidth(900));
  ui::ElementList all_rows;
  document->GetElementsByClassName(all_rows, "message-row");
  ui::Element* mine = nullptr;
  for (ui::Element* row : all_rows) {
    if (row->HasAttribute("message-id")) {
      mine = row;
      break;
    }
  }
  ASSERT_NE(mine, nullptr);
  mine->SetPseudoClass("hover", true);
  context->Update();
  ui::ElementList emoji_all;
  mine->GetElementsByClassName(emoji_all, "msg-bar-emoji");
  ui::ElementList emoji; // the data-for keeps its template element, not shown
  for (ui::Element* e : emoji_all) {
    if (e->GetComputedValues().display() != ui::Style::Display::None &&
        e->GetBox().GetSize(ui::BoxArea::Border).x > 0.f) {
      emoji.push_back(e);
    }
  }
  ASSERT_EQ(emoji.size(), 5u) << "five reactions";
  ui::ElementList others;
  mine->GetElementsByClassName(others, "msg-bar-btn");
  ASSERT_EQ(others.size(), 4u) << "+, Edit (our own text row), Reply, More";
  EXPECT_LT(BoxOf(emoji.back()).right, BoxOf(others[0]).left + 0.5f) << "the + comes after the reactions";
  EXPECT_LT(BoxOf(others[0]).right, BoxOf(others[1]).left + 0.5f) << "then Edit";
  EXPECT_LT(BoxOf(others[1]).right, BoxOf(others[2]).left + 0.5f) << "then Reply";
  EXPECT_LT(BoxOf(others[2]).right, BoxOf(others[3]).left + 0.5f) << "then More";
  EXPECT_LE(BoxOf(First("msg-bar")).H(), 40.f) << "one row";
  // Every slot draws its emoji: a glyph span with the emoji as its text, a box of its own, an opaque text colour, and the
  // same font family as the phone's quick-reaction row (the menu label), so the emoji font fallback applies alike.
  // The family is compared in a plain document (the app's body font), the bar's glyph beside the phone's menu label.
  const std::string probe_rml =
      "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
      Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") +
      "</style></head><body><div class=\"shell-body shell-body--wide\"><div class=\"context-menu-quick\">"
      "<div class=\"context-menu-item\"><span id=\"phone-label\" class=\"context-menu-item-label\">\xF0\x9F\x91\x8D"
      "</span></div></div><div class=\"msg-bar\"><button class=\"msg-bar-emoji\"><span id=\"bar-glyph\" "
      "class=\"msg-bar-emoji-glyph\">\xF0\x9F\x91\x8D</span></button></div></div></body></rml>";
  ui::ElementDocument* probe = context->LoadDocumentFromMemory(probe_rml);
  ASSERT_NE(probe, nullptr);
  probe->Show();
  context->Update();
  const ui::String phone_family = probe->GetElementById("phone-label")->GetProperty<ui::String>("font-family");
  EXPECT_FALSE(phone_family.empty());
  EXPECT_EQ(probe->GetElementById("bar-glyph")->GetProperty<ui::String>("font-family"), phone_family)
      << "the bar's emoji resolves to the phone quick row's font family";
  probe->Close();
  for (ui::Element* slot : emoji) {
    ui::ElementList glyphs;
    slot->GetElementsByClassName(glyphs, "msg-bar-emoji-glyph");
    ASSERT_EQ(glyphs.size(), 1u);
    ui::Element* glyph = glyphs[0];
    EXPECT_FALSE(glyph->GetInnerRML().empty()) << "the slot has its emoji as text";
    EXPECT_GT(glyph->GetBox().GetSize(ui::BoxArea::Border).x, 0.f) << "and a box of its own";
    EXPECT_EQ(glyph->GetComputedValues().color().alpha, 255) << "an opaque text colour";
    EXPECT_GT(glyph->GetComputedValues().font_size(), 14.f);
  }
}

TEST_F(ChatWideLayoutTest, TheChatPageRowsKeepTheirBindingsAndTheirOwnChat) {
  // chat_wide.rml is chat.rml plus the header's search button and the hover bar: every binding of the phone page is
  // still there, so the two pages stay one logic.
  const std::string phone = Asset("views/chat.rml");
  const std::string wide = Asset("views/chat_wide.rml");
  for (const char* binding : {"data-event-click=\"start_call()\"", "data-event-click=\"open_thread_actions_menu()\"",
                              "data-event-click=\"jump_to_latest()\"", "data-rml=\"row.content_rml\"",
                              "data-for=\"row : messages\"", "data-event-click=\"close_search()\""}) {
    EXPECT_NE(phone.find(binding), std::string::npos) << binding;
    EXPECT_NE(wide.find(binding), std::string::npos) << binding;
  }
  EXPECT_NE(wide.find("data-event-click=\"open_search()\""), std::string::npos);
  EXPECT_EQ(phone.find("msg-bar"), std::string::npos) << "the phone page is untouched";
}

TEST_F(ChatWideLayoutTest, TheChatsListRowIsAnAvatarAndTwoLinesAndTheSelectedRowIsFilled) {
  const std::string rml =
      "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
      Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") +
      "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
      "<div class=\"shell-body shell-body--wide\" style=\"position: absolute; width: 320dp; height: 600dp;\">"
      "<div class=\"sidebar-panel\"><div class=\"sidebar-sessions\">"
      "<div id=\"r1\" class=\"sidebar-session sidebar-session-active sidebar-session--private\">"
      "<div class=\"chat-row-avatar avatar-tone-2\"><p class=\"chat-row-avatar-letter\">A</p></div>"
      "<div class=\"sidebar-session-icon\"></div>"
      "<div class=\"sidebar-session-body\"><div class=\"sidebar-session-line\">"
      "<p class=\"sidebar-session-title\">Ann Lee</p><span class=\"sidebar-session-date\">10:42</span></div>"
      "<div class=\"sidebar-session-line\"><p class=\"sidebar-session-preview\">See you tomorrow</p>"
      "<span class=\"badge-count sidebar-unread\">3</span></div></div></div>"
      "<div id=\"r2\" class=\"sidebar-session\"><div class=\"chat-row-avatar avatar-tone-4\">"
      "<p class=\"chat-row-avatar-letter\">B</p></div><div class=\"sidebar-session-icon\"></div>"
      "<div class=\"sidebar-session-body\"><div class=\"sidebar-session-line\">"
      "<p class=\"sidebar-session-title\">Bob</p></div></div></div>"
      "</div></div></div></body></rml>";
  context->ActivateTheme("light", true);
  document = context->LoadDocumentFromMemory(rml);
  ASSERT_NE(document, nullptr);
  document->Show();
  context->Update();
  context->Update();
  ui::Element* selected = document->GetElementById("r1");
  ui::Element* plain = document->GetElementById("r2");
  ASSERT_NE(selected, nullptr);
  ASSERT_NE(plain, nullptr);
  EXPECT_GE(BoxOf(selected).H(), 68.f);
  ui::ElementList avatars;
  selected->GetElementsByClassName(avatars, "chat-row-avatar");
  ASSERT_EQ(avatars.size(), 1u);
  EXPECT_FLOAT_EQ(BoxOf(avatars[0]).W(), 48.f);
  EXPECT_FLOAT_EQ(BoxOf(avatars[0]).H(), 48.f);
  ui::ElementList icons;
  selected->GetElementsByClassName(icons, "sidebar-session-icon");
  EXPECT_EQ(icons[0]->GetComputedValues().display(), ui::Style::Display::None) << "the old type tile is gone";
  // The selected row is a filled, rounded surface; the others have no fill and no outline.
  EXPECT_GT(selected->GetComputedValues().background_color().alpha, 0);
  EXPECT_EQ(plain->GetComputedValues().background_color().alpha, 0);
  EXPECT_EQ(selected->GetComputedValues().border_top_width(), 0.f);
  EXPECT_FLOAT_EQ(selected->GetComputedValues().border_top_left_radius(), 10.f);
  // The title and the time share a line, the badge ends the second line, inside the row.
  ui::ElementList badge;
  selected->GetElementsByClassName(badge, "sidebar-unread");
  ASSERT_EQ(badge.size(), 1u);
  EXPECT_LE(BoxOf(badge[0]).right, BoxOf(selected).right);
  EXPECT_FLOAT_EQ(BoxOf(badge[0]).H(), 20.f);
}

TEST_F(ChatWideLayoutTest, EveryBubbleKindHasItsTailAnchoredToTheBubblesBottomCorner) {
  // Every kind AddBubbleTail decorates (text, a quoted reply, a picture, a video, a file card, a voice card; received and
  // sent): the hook element is drawn, 16 x 20dp, with its bottom on the bubble's bottom edge and 8dp outside its side
  // (left for received, right for sent), exactly as on the phone (a card's 1dp border moves it in by 1dp, as there).
  const std::string in_tail =
      "<svg class=\"bubble-tail bubble-tail-in\" src=\"../icons/v2/bubble-tail-left.svg\" width=\"16\" "
      "height=\"20\"></svg>";
  const std::string out_tail =
      "<svg class=\"bubble-tail bubble-tail-out\" src=\"../icons/v2/bubble-tail-right.svg\" width=\"16\" "
      "height=\"20\"></svg>";
  struct Kind {
    const char* open;
    const char* classes;
  };
  const Kind kinds[] = {{"div", "bubble bubble-assistant"},
                        {"div", "bubble bubble-assistant bubble-quote"},
                        {"div", "chat-card chat-attachment-card chat-attachment-card--image"},
                        {"div", "chat-card chat-attachment-card chat-attachment-card--video"},
                        {"div", "chat-card chat-attachment-card chat-file-card"},
                        {"div", "chat-card chat-voice-card"}};
  std::string body;
  int n = 0;
  for (const Kind& kind : kinds) {
    for (const bool ours : {false, true}) {
      body += std::string("<div class=\"message-row ") + (ours ? "message-row-user" : "message-row-peer") +
              "\"><div id=\"b" + std::to_string(n++) + "\" class=\"" + kind.classes + " bubble-tail-host " +
              (ours ? "bubble-tail-host--out" : "bubble-tail-host--in") + "\">" + (ours ? out_tail : in_tail) +
              "<p class=\"bubble-text\">x</p></div></div>";
    }
  }
  const std::string rml =
      "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
      Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") + Asset("themes/colors-dark.rcss") +
      "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
      "<div class=\"shell-body shell-body--wide\" style=\"position: absolute; width: 700dp; height: 900dp;\">" +
      body + "</div></body></rml>";
  for (const bool dark : {false, true}) {
    context->ActivateTheme("light", !dark);
    context->ActivateTheme("dark", dark);
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    context->Update();
    ui::ElementList tails;
    document->GetElementsByClassName(tails, "bubble-tail");
    ASSERT_EQ(tails.size(), 12u);
    for (int i = 0; i < n; ++i) {
      ui::Element* bubble = document->GetElementById("b" + std::to_string(i));
      ASSERT_NE(bubble, nullptr);
      const bool ours = i % 2 == 1;
      const Box host = BoxOf(bubble);
      ui::ElementList found;
      bubble->GetElementsByClassName(found, "bubble-tail");
      ASSERT_EQ(found.size(), 1u);
      ui::Element* tail = found[0];
      EXPECT_NE(tail->GetComputedValues().display(), ui::Style::Display::None) << "bubble " << i;
      const Box t = BoxOf(tail);
      EXPECT_FLOAT_EQ(t.W(), 16.f) << "bubble " << i;
      EXPECT_FLOAT_EQ(t.H(), 20.f) << "bubble " << i;
      EXPECT_NEAR(t.bottom, host.bottom, 1.5f) << "the tail sits on the bottom edge of bubble " << i;
      if (ours) {
        EXPECT_NEAR(t.right, host.right + 8.f, 1.5f) << "bubble " << i << " dark " << dark;
      } else {
        EXPECT_NEAR(t.left, host.left - 8.f, 1.5f) << "bubble " << i << " dark " << dark;
      }
    }
    document->Close();
    context->Update();
  }
}

TEST_F(ChatWideLayoutTest, SystemLinesAndDatePillsAreSmallPillsSizedToTheirTextAndCentred) {
  // As on the phone: member joined / left, call records' neighbours and the date strips are grey pills round their text,
  // centred, at most 80% of the column; never a row stretched over the whole conversation.
  const std::string body =
      "<div id=\"col\" style=\"width: 700dp;\">"
      "<div class=\"chat-system-line muted\"><p id=\"sys\">Ann joined the group</p></div>"
      "<div class=\"chat-date-strip\"><span id=\"day\" class=\"chat-date-pill\">Today</span></div>"
      "<div class=\"chat-system-line muted\"><p id=\"long\">" +
      std::string(400, 'x') + "</p></div></div>";
  const std::string rml =
      "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
      Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") +
      "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
      "<div class=\"shell-body shell-body--wide\" style=\"position: absolute; width: 800dp; height: 600dp;\">" +
      body + "</div></body></rml>";
  document = context->LoadDocumentFromMemory(rml);
  ASSERT_NE(document, nullptr);
  document->Show();
  context->Update();
  context->Update();
  const Box col = BoxOf(document->GetElementById("col"));
  for (const char* id : {"sys", "day"}) {
    const Box b = BoxOf(document->GetElementById(id));
    EXPECT_LT(b.W(), 300.f) << id << " is sized to its text";
    EXPECT_NEAR((b.left + b.right) / 2.f, (col.left + col.right) / 2.f, 1.f) << id << " is centred";
    EXPECT_FLOAT_EQ(document->GetElementById(id)->GetComputedValues().border_top_left_radius(), 999.f);
  }
  EXPECT_LE(BoxOf(document->GetElementById("long")).W(), col.W() * 0.8f + 20.5f)
      << "the text is at most 80% of the column (its 10dp side padding comes on top, as on the phone)";
}

} // namespace
} // namespace pbr
