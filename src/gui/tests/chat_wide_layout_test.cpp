// The chat page in the wide layout (design step 2): chat_wide.rml with the real stylesheets, in a conversation column
// at the widths the window gives it at 768 / 900 / 1280. Header buttons, search in place, the hover bar and the chats
// list row, measured; plus the hover bar offering exactly what the message menu offers.
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/ChatMenus.h"
#include "gui/chat/MessageMenuSlots.h"
#include "gui/chat/TranscriptRml.h"
#include "lib/delivery/DeliveryNotice.h"
#include "lib/ui/ViewCatalog.h"
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
#include <map>
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
    mine.bar_react =
        true; // a 1:1 chat takes reactions; the bar draws what the message menu offers (MessageMenuSlots.h)
    mine.bar_reply = true;
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
  ChatShellState shell; ///< the Chats list column's model ("shell"), for the tests that mount sidebar.rml
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
  auto hover_of = [](ui::Element* row) -> ui::Element* {
    ui::ElementList found;
    row->GetElementsByClassName(found, "msg-hover");
    return found.empty() ? nullptr : found[0];
  };
  for (size_t i = 0; i < 3; ++i) {
    ASSERT_NE(hover_of(rows[i]), nullptr) << "the hover target wraps the message's content and its bar: " << i;
    hover_of(rows[i])->SetPseudoClass("hover", true);
    context->Update();
    ui::Element* bar = bar_of(rows[i]);
    if (i < 2) {
      EXPECT_NE(bar->GetComputedValues().display(), ui::Style::Display::None) << "shown under the pointer: " << i;
      EXPECT_GE(BoxOf(bar).H(), 28.f);
      EXPECT_LE(BoxOf(bar).right, BoxOf(rows[i]).right + 0.5f) << "the bar stays inside the row's width";
    } else {
      EXPECT_EQ(bar->GetComputedValues().display(), ui::Style::Display::None) << "a notice row has no bar";
    }
    hover_of(rows[i])->SetPseudoClass("hover", false);
    context->Update();
  }
}

// The bar's hover target is the bubble (owner 2026-11-21): the row's empty space beside it, the avatar and the name do not
// show it; it stays while the pointer moves from the bubble onto the bar; it is not drawn while the transcript scrolls.
// Hit testing is the library's own (a pointer position over the laid-out page); a real pointer cannot be tried here.
TEST_F(ChatWideLayoutTest, TheHoverBarFollowsTheBubbleNotTheRowAndNotWhileScrolling) {
  ShowChat();
  Load(ConversationWidth(900));
  ui::Element* peer = nullptr;
  ui::ElementList all_rows;
  document->GetElementsByClassName(all_rows, "message-row");
  for (ui::Element* row : all_rows) {
    if (row->GetAttribute("message-id", ui::String()) == "m2") {
      peer = row;
    }
  }
  ASSERT_NE(peer, nullptr);
  ui::ElementList found;
  peer->GetElementsByClassName(found, "bubble");
  ASSERT_FALSE(found.empty());
  ui::Element* bubble = found[0];
  found.clear();
  peer->GetElementsByClassName(found, "msg-bar");
  ASSERT_FALSE(found.empty());
  ui::Element* bar = found[0];
  auto move_to = [&](const float x, const float y) {
    context->ProcessMouseMove(static_cast<int>(x), static_cast<int>(y), 0);
    context->Update();
    context->Update();
  };
  auto shown = [&]() { return bar->GetComputedValues().display() != ui::Style::Display::None; };
  const Box b = BoxOf(bubble);
  const Box row = BoxOf(peer);
  ASSERT_LT(b.right + 60.f, row.right) << "the short message leaves empty row space to its right";

  move_to((b.left + b.right) / 2, (b.top + b.bottom) / 2);
  EXPECT_TRUE(shown()) << "the pointer is on the bubble";
  // The bar overlaps the bubble's top edge, so the pointer can go up onto it without leaving the hover area.
  const Box bar_box = BoxOf(bar);
  EXPECT_LT(bar_box.top, b.top);
  EXPECT_GT(bar_box.bottom, b.top) << "no gap between the bubble and the bar";
  move_to((bar_box.left + bar_box.right) / 2, (bar_box.top + bar_box.bottom) / 2);
  EXPECT_TRUE(shown()) << "still shown with the pointer on the bar";

  move_to(row.right - 20.f, (b.top + b.bottom) / 2);
  EXPECT_FALSE(shown()) << "the empty row space beside the bubble does not show it";

  // Scrolling: one class on the transcript hides every bar, whatever the pointer is over.
  move_to((b.left + b.right) / 2, (b.top + b.bottom) / 2);
  ASSERT_TRUE(shown());
  ui::Element* messages = document->GetElementById("chat-messages");
  ASSERT_NE(messages, nullptr);
  messages->SetClass("messages--scrolling", true);
  context->Update();
  EXPECT_FALSE(shown()) << "no bar while the transcript scrolls";
  messages->SetClass("messages--scrolling", false);
  context->Update();
  EXPECT_TRUE(shown()) << "back when the scroll has been quiet";
}

// The hover target holds the message's content and its bar only: the name above and the avatar beside it are outside.
TEST_F(ChatWideLayoutTest, TheHoverTargetHoldsTheContentAndTheBarButNotTheNameOrTheAvatar) {
  const std::string page = Asset("views/chat_wide.rml");
  const size_t at = page.find("<div class=\"msg-hover\">");
  ASSERT_NE(at, std::string::npos);
  const std::string wrapped = page.substr(at, page.find("class=\"message-group\"", at) - at);
  EXPECT_NE(wrapped.find("data-rml=\"row.content_rml\""), std::string::npos);
  EXPECT_NE(wrapped.find("class=\"msg-bar\""), std::string::npos);
  EXPECT_EQ(wrapped.find("chat-group-name"), std::string::npos);
  EXPECT_EQ(wrapped.find("chat-row-avatar"), std::string::npos);
  EXPECT_EQ(wrapped.find("sender-label"), std::string::npos);
  // The scroll class is the transcript's own, and the bar rule that it hides is more specific than the one that shows it.
  const std::string css = Asset("themes/wide.rcss");
  EXPECT_NE(css.find(".shell-body--wide .messages--scrolling .msg-hover:hover > .msg-bar"), std::string::npos);
}

// The bar draws what the menu offers (MessageMenuSlots.h): by conversation kind, and by the message.
TEST(MessageMenuSlotsTest, ReactionsOnlyWhereThereIsSomeoneToSendThemTo) {
  EXPECT_TRUE(ThreadTakesReactions(ThreadKind::Direct));
  EXPECT_TRUE(ThreadTakesReactions(ThreadKind::Group));
  EXPECT_FALSE(ThreadTakesReactions(ThreadKind::Ai)) << "the AI would not see them";
  // Direct and group: the whole set; AI: Reply, Copy, Edit but no reactions.
  for (const ThreadKind kind : {ThreadKind::Direct, ThreadKind::Group}) {
    const MessageMenuSlots slots = DecideMessageMenuSlots(ThreadTakesReactions(kind), true, true, false);
    EXPECT_TRUE(slots.react);
    EXPECT_TRUE(slots.reply);
    EXPECT_TRUE(slots.copy);
    EXPECT_TRUE(slots.edit);
  }
  const MessageMenuSlots ai = DecideMessageMenuSlots(ThreadTakesReactions(ThreadKind::Ai), true, false, false);
  EXPECT_FALSE(ai.react);
  EXPECT_TRUE(ai.reply);
  EXPECT_TRUE(ai.copy);
  EXPECT_FALSE(ai.edit);
  // A message with no words (a picture, a file): no Reply or Copy; reactions as the conversation allows.
  const MessageMenuSlots picture = DecideMessageMenuSlots(true, false, false, false);
  EXPECT_TRUE(picture.react);
  EXPECT_FALSE(picture.reply);
  EXPECT_FALSE(picture.copy);
  const MessageMenuSlots ai_picture = DecideMessageMenuSlots(false, false, false, false);
  EXPECT_FALSE(ai_picture.react || ai_picture.reply || ai_picture.copy || ai_picture.edit) << "only More is left";
  // While a message is being edited: no Reply or Edit; reactions and Copy stay.
  const MessageMenuSlots editing = DecideMessageMenuSlots(true, true, true, true);
  EXPECT_TRUE(editing.react);
  EXPECT_TRUE(editing.copy);
  EXPECT_FALSE(editing.reply);
  EXPECT_FALSE(editing.edit);
}

// The menu and the bar are built from the one decision: the menu's reaction row, Reply, Copy and Edit follow `slots`, and
// the bar's rows are filled from the same function (ChatController's bar_offer), so no slot is drawn that the menu lacks.
TEST_F(ChatWideLayoutTest, TheMenuAndTheBarShareTheOneDecision) {
  const std::string menus = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatMenus.cpp");
  const size_t for_request = menus.find("std::vector<ContextMenuAction> ChatMenus::ForRequest(");
  ASSERT_NE(for_request, std::string::npos);
  const size_t decide = menus.find("DecideMessageMenuSlots(", for_request);
  ASSERT_NE(decide, std::string::npos);
  EXPECT_NE(menus.find("if (slots.react)", decide), std::string::npos);
  EXPECT_NE(menus.find("if (slots.reply)", decide), std::string::npos);
  EXPECT_NE(menus.find("if (slots.copy)", decide), std::string::npos);
  EXPECT_NE(menus.find("} else if (slots.edit)", decide), std::string::npos);
  const std::string controller = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp");
  const size_t offer = controller.find("facts.bar_offer =");
  ASSERT_NE(offer, std::string::npos);
  EXPECT_NE(controller.find("DecideMessageMenuSlots(takes_reactions", offer), std::string::npos);
  // The picker path and a stray call do nothing in an AI conversation, quietly, and no protocol sentence is shown.
  const size_t toggle = controller.find("void ChatController::ToggleReaction(");
  ASSERT_NE(toggle, std::string::npos);
  const std::string body = controller.substr(toggle, 900);
  EXPECT_NE(body.find("!ThreadTakesReactions(open_thread->kind)"), std::string::npos);
  EXPECT_EQ(body.find("sent.error().message, ToastDuration"), std::string::npos) << "the raw error is not a pill";
}

// An AI conversation's bar: no reaction row, no "+", no separator; Reply and More (and Edit where offered). A message
// with nothing but More left shows only More; a conversation that takes reactions keeps the whole bar.
TEST_F(ChatWideLayoutTest, TheHoverBarShrinksToWhatTheMenuOffersInAnAiConversation) {
  ShowChat();
  for (const char* emoji : {"\xF0\x9F\x91\x8D", "\xE2\x9D\xA4", "\xF0\x9F\x98\x82"}) {
    BarEmojiRow row;
    row.emoji = emoji;
    state.bar_emojis.push_back(row);
  }
  state.messages[0].bar_react = false; // mine, AI conversation: Reply and Edit, no reactions
  state.messages[1].bar_react = false; // theirs: Reply and More
  state.messages[1].bar_reply = false; // ... and one with no words: only More
  Load(ConversationWidth(900));
  ui::ElementList wraps;
  document->GetElementsByClassName(wraps, "msg-hover");
  ASSERT_GE(wraps.size(), 2u);
  auto shown_in = [&](ui::Element* wrap, const char* cls) {
    ui::ElementList found;
    wrap->GetElementsByClassName(found, cls);
    size_t drawn = 0;
    for (ui::Element* e : found) {
      if (e->GetComputedValues().display() != ui::Style::Display::None && BoxOf(e).W() > 0.f) {
        ++drawn;
      }
    }
    return drawn;
  };
  for (size_t i = 0; i < 2; ++i) {
    wraps[i]->SetPseudoClass("hover", true);
  }
  context->Update();
  EXPECT_EQ(shown_in(wraps[0], "msg-bar-emoji"), 0u) << "no emoji in an AI conversation";
  EXPECT_EQ(shown_in(wraps[0], "msg-bar-sep"), 0u);
  EXPECT_EQ(shown_in(wraps[0], "msg-bar-btn"), 3u) << "Edit, Reply, More: the plus is gone";
  EXPECT_EQ(shown_in(wraps[1], "msg-bar-emoji"), 0u);
  EXPECT_EQ(shown_in(wraps[1], "msg-bar-btn"), 1u) << "nothing but More";
  const Box full_less = BoxOf(First("msg-bar"));
  EXPECT_LE(full_less.H(), 40.f);
  // A conversation that takes reactions: the whole bar.
  document->Close();
  context->Update();
  ShowChat();
  state.bar_emojis.clear();
  for (const char* emoji : {"\xF0\x9F\x91\x8D", "\xE2\x9D\xA4", "\xF0\x9F\x98\x82"}) {
    BarEmojiRow row;
    row.emoji = emoji;
    state.bar_emojis.push_back(row);
  }
  Load(ConversationWidth(900));
  document->GetElementsByClassName(wraps, "msg-hover");
  ui::ElementList direct_wraps;
  document->GetElementsByClassName(direct_wraps, "msg-hover");
  ASSERT_GE(direct_wraps.size(), 1u);
  direct_wraps[0]->SetPseudoClass("hover", true);
  context->Update();
  EXPECT_EQ(shown_in(direct_wraps[0], "msg-bar-emoji"), 3u);
  EXPECT_EQ(shown_in(direct_wraps[0], "msg-bar-btn"), 4u) << "+, Edit, Reply, More";
  EXPECT_GE(BoxOf(First("msg-bar")).W(), full_less.W()) << "the AI bar is the shorter one";
}

TEST_F(ChatWideLayoutTest, TheHoverBarOffersExactlyTheMessageMenusItems) {
  // The used reactions (one button repeated over bar_emojis), "+" (the full picker), Reply and More: the reactions and
  // Reply run the message menu's own items, More opens that whole menu.
  const std::string page = Asset("views/chat_wide.rml");
  const size_t bar = page.find("class=\"msg-bar\"");
  ASSERT_NE(bar, std::string::npos);
  const std::string bar_markup = page.substr(bar, page.find("class=\"message-group\"", bar) - bar);
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
  ui::ElementList wraps;
  mine->GetElementsByClassName(wraps, "msg-hover");
  ASSERT_EQ(wraps.size(), 1u);
  wraps[0]->SetPseudoClass("hover", true);
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

// The floating menu is one raised card on the wide layout: large corners, a clear surface, and one line (with a little
// air) above the destructive group only; the phone's own menus are left alone.
TEST_F(ChatWideLayoutTest, TheFloatingMenuIsOneRaisedCardWithOneLineAboveTheDestructiveGroup) {
  const std::string menu_rml =
      "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
      Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") +
      "</style></head><body><div class=\"shell-body shell-body--wide\"><div class=\"context-menu-layer\">"
      "<div class=\"context-menu-panel\" id=\"context-menu-panel\" style=\"left: 40px; top: 40px;\">"
      "<button class=\"context-menu-item\" id=\"m-plain\"><span "
      "class=\"context-menu-item-label\">Search</span></button>"
      "<button class=\"context-menu-item context-menu-item--danger\" id=\"m-danger1\"><span "
      "class=\"context-menu-item-label\">Clear</span></button>"
      "<button class=\"context-menu-item context-menu-item--danger\" id=\"m-danger2\"><span "
      "class=\"context-menu-item-label\">Forget</span></button>"
      "</div></div></div></body></rml>";
  ui::ElementDocument* menu = context->LoadDocumentFromMemory(menu_rml);
  ASSERT_NE(menu, nullptr);
  menu->Show();
  context->Update();
  ui::Element* panel = menu->GetElementById("context-menu-panel");
  ui::Element* plain = menu->GetElementById("m-plain");
  ui::Element* danger1 = menu->GetElementById("m-danger1");
  ui::Element* danger2 = menu->GetElementById("m-danger2");
  ASSERT_TRUE(panel && plain && danger1 && danger2);
  const float dp = context->GetDensityIndependentPixelRatio();
  EXPECT_GE(panel->GetComputedValues().border_top_left_radius(), 16.f * dp - 0.5f) << "large corners";
  const Box a = BoxOf(plain);
  const Box b = BoxOf(danger1);
  const Box c = BoxOf(danger2);
  EXPECT_GE(b.top - a.bottom, 5.f * dp - 0.5f) << "air above the destructive group";
  EXPECT_NEAR(c.top, b.bottom, 0.5f) << "no gap inside the group";
  EXPECT_GT(danger1->GetComputedValues().border_top_width(), 0.f) << "a line above the group";
  EXPECT_EQ(danger2->GetComputedValues().border_top_width(), 0.f) << "none inside it";
  EXPECT_EQ(plain->GetComputedValues().border_top_width(), 0.f);
  const ui::String shadow = panel->GetProperty<ui::String>("box-shadow");
  EXPECT_FALSE(shadow.empty() || shadow == "none") << "a shadow under the card";
  menu->Close();
}

// The full emoji picker (the plus in the reaction row) opens in the shell's frame with the key emoji_picker: on the wide
// layout it is the same raised card as the floating menus (large corners, a shadow, a raised surface in dark); under
// the phone root the frame is left alone.
TEST_F(ChatWideLayoutTest, TheEmojiPickerFrameIsARaisedCardOnTheWideLayoutOnly) {
  auto frame_of = [&](const char* root_class, const char* theme) {
    context->ActivateTheme("light", theme == std::string("light")); // the colours live under @media (theme: ...)
    context->ActivateTheme("dark", theme == std::string("dark"));
    const std::string colors = std::string("themes/colors-") + theme + ".rcss";
    const std::string rml = "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
                            Asset("themes/wide.rcss") + Asset(colors.c_str()) + "</style></head><body><div class=\"" +
                            root_class +
                            "\" style=\"width: 900px; height: 600px;\"><div class=\"shell-layer shell-layer-overlay\">"
                            "<div class=\"shell-frame shell-frame--emoji_picker\" id=\"frame\"></div></div></div>"
                            "</body></rml>";
    ui::ElementDocument* doc = context->LoadDocumentFromMemory(rml);
    EXPECT_NE(doc, nullptr);
    doc->Show();
    context->Update();
    return doc;
  };
  const float dp = context->GetDensityIndependentPixelRatio();
  ui::ElementDocument* wide = frame_of("shell-body shell-body--wide", "light");
  ASSERT_NE(wide, nullptr);
  ui::Element* wide_frame = wide->GetElementById("frame");
  ASSERT_NE(wide_frame, nullptr);
  EXPECT_GE(wide_frame->GetComputedValues().border_top_left_radius(), 16.f * dp - 0.5f);
  const ui::String wide_shadow = wide_frame->GetProperty<ui::String>("box-shadow");
  EXPECT_FALSE(wide_shadow.empty() || wide_shadow == "none") << "a shadow under the card";
  const ui::Colourb light_bg = wide_frame->GetComputedValues().background_color();
  wide->Close();

  ui::ElementDocument* dark = frame_of("shell-body shell-body--wide", "dark");
  ASSERT_NE(dark, nullptr);
  const ui::Colourb dark_bg = dark->GetElementById("frame")->GetComputedValues().background_color();
  EXPECT_NE(dark_bg, light_bg);
  dark->Close();

  ui::ElementDocument* phone = frame_of("shell-body", "light");
  ASSERT_NE(phone, nullptr);
  ui::Element* phone_frame = phone->GetElementById("frame");
  ASSERT_NE(phone_frame, nullptr);
  EXPECT_LT(phone_frame->GetComputedValues().border_top_left_radius(), 16.f * dp - 0.5f)
      << "unchanged under the phone root";
  const ui::String phone_shadow = phone_frame->GetProperty<ui::String>("box-shadow");
  EXPECT_TRUE(phone_shadow.empty() || phone_shadow == "none") << "no shadow under the phone root";
  phone->Close();
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
      // Drawn in front of the pane's surface: a negative z-index (the phone's, behind a picture card) hid the picture
      // card's hook in the wide transcript.
      EXPECT_GE(tail->GetComputedValues().z_index().value, 0.f) << "bubble " << i;
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

// The macOS window's outer corner (borderless, transparent, shaped by WindowChrome_Darwin.mm): the 1dp outline follows
// an 18dp radius; a maximized or full-screen window (no `framed` class) is square as before, and a window that is not the
// macOS one (Windows, Linux: no `round` class) keeps its square outline.
TEST_F(ChatWideLayoutTest, TheMacWindowOutlineFollowsTheRoundedOuterCorner) {
  const std::string mm =
      ReadAll(std::string(PP_SOURCE_DIR) + "/src/foundation/platform/desktop/WindowChrome_Darwin.mm");
  EXPECT_NE(mm.find("kWindowCornerRadiusPt = 18.0"), std::string::npos);
  EXPECT_NE(Asset("samples/window_shell.rml").find("data-class-shell-body--round=\"titlebar_traffic_lights\""),
            std::string::npos);
  const std::string rml =
      "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
      Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") +
      "</style></head><body style=\"padding: 0;\">"
      "<div id=\"mac\" class=\"shell-body shell-body--framed shell-body--round\" style=\"width: 400dp; height: "
      "300dp;\"></div>"
      "<div id=\"win\" class=\"shell-body shell-body--framed\" style=\"width: 400dp; height: 300dp;\"></div>"
      "<div id=\"max\" class=\"shell-body shell-body--round\" style=\"width: 400dp; height: 300dp;\"></div>"
      "</body></rml>";
  document = context->LoadDocumentFromMemory(rml);
  ASSERT_NE(document, nullptr);
  document->Show();
  context->Update();
  EXPECT_FLOAT_EQ(document->GetElementById("mac")->GetComputedValues().border_top_left_radius(), 18.f);
  EXPECT_FLOAT_EQ(document->GetElementById("mac")->GetComputedValues().border_bottom_right_radius(), 18.f);
  EXPECT_FLOAT_EQ(document->GetElementById("win")->GetComputedValues().border_top_left_radius(), 0.f);
  EXPECT_FLOAT_EQ(document->GetElementById("max")->GetComputedValues().border_top_left_radius(), 0.f);
}

// Windows 11 rounds our borderless window itself when asked (WindowCorners_Win32.cpp: DWMWA_WINDOW_CORNER_PREFERENCE 33
// = DWMWCP_ROUND 2, dwmapi loaded at run time; the other desktops answer no). When it did (`window_corners_small`), the
// 1dp outline follows the system's small radius (8), not the Mac's 18; maximized (no `framed` class) it is square, and
// Windows 10 (the request refused: no `round-small` class) keeps the square outline.
TEST_F(ChatWideLayoutTest, TheWindows11OutlineFollowsTheSystemsSmallCorner) {
  const std::string win32 =
      ReadAll(std::string(PP_SOURCE_DIR) + "/src/foundation/platform/desktop/WindowCorners_Win32.cpp");
  EXPECT_NE(win32.find("kDwmwaWindowCornerPreference = 33"), std::string::npos);
  EXPECT_NE(win32.find("kDwmwcpRound = 2"), std::string::npos);
  EXPECT_NE(win32.find("LoadLibraryExW(L\"dwmapi.dll\""), std::string::npos) << "nothing new is linked";
  const std::string cmake = ReadAll(std::string(PP_SOURCE_DIR) + "/src/foundation/platform/CMakeLists.txt");
  size_t defaults = 0;
  for (size_t at = cmake.find("desktop/WindowCorners_Default.cpp"); at != std::string::npos;
       at = cmake.find("desktop/WindowCorners_Default.cpp", at + 1)) {
    ++defaults;
  }
  EXPECT_NE(cmake.find("desktop/WindowCorners_Win32.cpp"), std::string::npos);
  EXPECT_EQ(defaults, 2u) << "the Darwin branch and the Linux / Android one";
  EXPECT_NE(Asset("samples/window_shell.rml").find("data-class-shell-body--round-small=\"window_corners_small\""),
            std::string::npos);
  const std::string rml =
      "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
      Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") +
      "</style></head><body style=\"padding: 0;\">"
      "<div id=\"win11\" class=\"shell-body shell-body--framed shell-body--round-small\" style=\"width: 400dp; "
      "height: 300dp;\"></div>"
      "<div id=\"win10\" class=\"shell-body shell-body--framed\" style=\"width: 400dp; height: 300dp;\"></div>"
      "<div id=\"max\" class=\"shell-body shell-body--round-small\" style=\"width: 400dp; height: 300dp;\"></div>"
      "</body></rml>";
  document = context->LoadDocumentFromMemory(rml);
  ASSERT_NE(document, nullptr);
  document->Show();
  context->Update();
  const auto& win11 = document->GetElementById("win11")->GetComputedValues();
  EXPECT_FLOAT_EQ(win11.border_top_left_radius(), 8.f);
  EXPECT_FLOAT_EQ(win11.border_top_right_radius(), 8.f);
  EXPECT_FLOAT_EQ(win11.border_bottom_left_radius(), 8.f);
  EXPECT_FLOAT_EQ(win11.border_bottom_right_radius(), 8.f);
  EXPECT_FLOAT_EQ(win11.border_top_width(), 1.f) << "the outline itself";
  EXPECT_FLOAT_EQ(document->GetElementById("win10")->GetComputedValues().border_top_left_radius(), 0.f);
  EXPECT_FLOAT_EQ(document->GetElementById("max")->GetComputedValues().border_top_left_radius(), 0.f);
  EXPECT_FLOAT_EQ(document->GetElementById("max")->GetComputedValues().border_top_width(), 0.f);
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

// B236 in the wide conversation pane: the "held" line (a chat the recipient has not added) is the phone's small centred
// pill, and our messages' marks (a clock while sending, the 44dp red mark when it did not go out) sit just left of their
// bubbles, inside the column. The rows are the real transcript's (RenderTranscript), not hand-written markup.
TEST_F(ChatWideLayoutTest, TheHeldLineAndTheSendMarksShowInTheWideConversation) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("en");
  const auto ours = [](const std::string& id, const int64_t at) {
    TranscriptRow row;
    row.message.id = id;
    row.message.thread_id = "t";
    row.message.sender_contact_id = kLocalSelfContactId;
    row.message.content_type = ChatContentType::Text;
    row.message.text = "hello";
    row.message.timestamp = at;
    return row;
  };
  TranscriptRow held;
  held.message = MakeStrangerHeldRow("t", "queued", 3);
  const std::vector<TranscriptRow> entries = {ours("failed", 1), ours("queued", 2), held};
  const std::map<std::string, SendMark> marks = {{"failed", SendMark::Failed}, {"queued", SendMark::Queued}};
  TranscriptFacts facts;
  facts.send_mark = [marks](const ThreadMessage& message) {
    const auto it = marks.find(message.id);
    return it == marks.end() ? SendMark::None : it->second;
  };
  for (const int window : {768, 1280}) {
    SCOPED_TRACE(window);
    const int column = ConversationWidth(window);
    ShowChat();
    state.messages = RenderTranscript(entries, facts);
    ASSERT_EQ(state.messages.size(), 3u);
    Load(column);
    const Box col = BoxOf(document->GetElementById("col"));

    ui::ElementList lines;
    document->QuerySelectorAll(lines, ".chat-system-line p");
    ASSERT_EQ(lines.size(), 1u) << "one held line";
    EXPECT_EQ(lines[0]->GetInnerRML(), Tr("chat.send.held_line"));
    const Box line = BoxOf(lines[0]);
    EXPECT_GT(line.H(), 0.f) << "drawn";
    EXPECT_GE(line.left, col.left);
    EXPECT_LE(line.right, col.right);
    EXPECT_FLOAT_EQ(lines[0]->GetComputedValues().border_top_left_radius(), 999.f) << "the phone's pill";

    for (const char* cls : {"send-mark--failed", "send-mark--queued"}) {
      SCOPED_TRACE(cls);
      ui::Element* mark = First(cls);
      ASSERT_NE(mark, nullptr);
      const Box m = BoxOf(mark);
      ui::ElementList bubbles;
      mark->GetParentNode()->GetElementsByClassName(bubbles, "bubble");
      ASSERT_EQ(bubbles.size(), 1u) << "the mark sits beside its own bubble";
      const Box b = BoxOf(bubbles[0]);
      ui::ElementList icons;
      mark->GetElementsByClassName(icons, "send-mark-icon");
      ASSERT_EQ(icons.size(), 1u);
      // The icon is left of the bubble (the red mark's 44dp target may reach 6dp under the bubble's edge, as on the phone).
      EXPECT_LE(BoxOf(icons[0]).right, b.left + 0.5f) << "left of the bubble";
      EXPECT_GT(BoxOf(icons[0]).W(), 0.f);
      EXPECT_GE(m.left, col.left);
      EXPECT_LE(b.right, col.right);
      EXPECT_GT(m.bottom, b.top) << "level with the bubble, not on a line of its own";
    }
    EXPECT_FLOAT_EQ(BoxOf(First("send-mark--failed")).W(), 44.f) << "a 44dp tap target";
    EXPECT_FLOAT_EQ(BoxOf(First("send-mark--failed")).H(), 44.f);
    EXPECT_FLOAT_EQ(BoxOf(First("send-mark--queued")).W(), 24.f);
    document->Close();
    context->Update();
  }
}

// The Chats list column of the wide layout is sidebar.rml: an imported account with no chats yet shows the spinner and
// "Syncing..." line there (not the "start a chat" empty state), inside the column, as on the phone.
TEST_F(ChatWideLayoutTest, TheChatsColumnShowsTheSyncIndicatorWhileTheAccountIsSyncing) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("en");
  ASSERT_TRUE(DataModelHost::Instance().Register(
      context, "shell", [this](ui::DataModelConstructor& ctor) { BindChatShellState(ctor, shell); }));
  const auto load = [this]() {
    const std::string list = ViewCatalog::LoadBody(ViewCatalog::KeyForLayout("sidebar", /*wide=*/true));
    const std::string rml =
        "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
        Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") +
        "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
        "<div class=\"shell-body shell-body--wide\" style=\"position: absolute; width: 1280dp; height: 700dp;\">"
        "<div class=\"shell-layer shell-layer-base\"><div id=\"list\" class=\"shell-pane shell-pane-secondary\" "
        "style=\"position: absolute; left: 64dp; width: 320dp; height: 700dp;\"><div class=\"shell-pane-body\">" +
        list + "</div></div></div></div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    context->Update();
  };
  const auto drawn = [this](const char* selector) {
    ui::ElementList found;
    document->QuerySelectorAll(found, selector);
    ui::ElementList shown;
    for (ui::Element* element : found) {
      if (BoxOf(element).H() > 0.f) {
        shown.push_back(element);
      }
    }
    return shown;
  };
  shell.account_sync_syncing = true;
  load();
  const Box column = BoxOf(document->GetElementById("list"));
  const ui::ElementList pending = drawn(".sync-pending");
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_GE(BoxOf(pending[0]).left, column.left);
  EXPECT_LE(BoxOf(pending[0]).right, column.right);
  ASSERT_EQ(drawn(".sync-pending-spinner").size(), 1u);
  const ui::ElementList line = drawn(".sync-pending p");
  ASSERT_EQ(line.size(), 1u);
  EXPECT_EQ(line[0]->GetInnerRML(), Tr("account_sync.pending"));
  EXPECT_EQ(drawn(".sidebar-empty").size(), 0u);
  document->Close();
  context->Update();

  shell.account_sync_syncing = false;
  load();
  EXPECT_EQ(drawn(".sync-pending").size(), 0u);
  EXPECT_EQ(drawn(".sidebar-empty").size(), 1u) << "the empty state once the sync is over";
}

// The Chats list column of the wide layout (sidebar.rml) with the device-removed notice (DeviceRemovedNotice): the first
// tip card, above the recovery reminder, the same card (width, corner, colour), inside the column.
TEST_F(ChatWideLayoutTest, TheChatsColumnShowsTheDeviceRemovedNoticeAboveTheOtherCards) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("en");
  ASSERT_TRUE(DataModelHost::Instance().Register(
      context, "shell", [this](ui::DataModelConstructor& ctor) { BindChatShellState(ctor, shell); }));
  shell.device_removed_notice = true;
  shell.recovery_reminder = true;
  context->ActivateTheme("light", true);
  const std::string list = ViewCatalog::LoadBody(ViewCatalog::KeyForLayout("sidebar", /*wide=*/true));
  const std::string rml =
      "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
      Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") +
      "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
      "<div class=\"shell-body shell-body--wide\" style=\"position: absolute; width: 1280dp; height: 700dp;\">"
      "<div class=\"shell-layer shell-layer-base\"><div id=\"list\" class=\"shell-pane shell-pane-secondary\" "
      "style=\"position: absolute; left: 64dp; width: 320dp; height: 700dp;\"><div class=\"shell-pane-body\">" +
      list + "</div></div></div></div></body></rml>";
  document = context->LoadDocumentFromMemory(rml);
  ASSERT_NE(document, nullptr);
  document->Show();
  context->Update();
  context->Update();
  ui::ElementList cards;
  for (ui::Element* card : All("sidebar-tip")) {
    if (BoxOf(card).H() > 0.f) {
      cards.push_back(card);
    }
  }
  ASSERT_EQ(cards.size(), 2u);
  const Box column = BoxOf(document->GetElementById("list"));
  const Box removed = BoxOf(cards[0]);
  const Box recovery = BoxOf(cards[1]);
  EXPECT_NE(std::string(cards[0]->GetInnerRML().c_str()).find(Tr("device_removed.relink")), std::string::npos);
  EXPECT_NE(std::string(cards[1]->GetInnerRML().c_str()).find(Tr("recovery_reminder.now")), std::string::npos);
  EXPECT_LT(removed.bottom, recovery.top + 0.5f) << "above the recovery reminder";
  EXPECT_GE(removed.left, column.left);
  EXPECT_LE(removed.right, column.right);
  EXPECT_FLOAT_EQ(removed.left, recovery.left);
  EXPECT_FLOAT_EQ(removed.W(), recovery.W());
  EXPECT_FLOAT_EQ(cards[0]->GetComputedValues().border_top_left_radius(),
                  cards[1]->GetComputedValues().border_top_left_radius());
  EXPECT_GT(cards[0]->GetComputedValues().background_color().alpha, 0) << "a filled card";
  EXPECT_EQ(cards[0]->GetComputedValues().background_color(), cards[1]->GetComputedValues().background_color());
}

// The news reader on the wide window (news_reader_wide.rml): a panel centred over the conversation in the shell's
// frame, 720dp at most, title on top (a translated digest's "AI translation of the original" line under it), a
// scrolling body, then one bar with "Open original" and Previous / Next. The phone's sheet view is untouched, and a news
// link hovers on the wide layout only.
TEST_F(ChatWideLayoutTest, TheNewsReaderIsACentredPanelWithItsTitleBodyAndBar) {
  state.news_reader_title = "Chip makers brace for new export rules";
  state.news_reader_translated = true;
  state.news_reader_body_rml = "<p>The digest.</p>";
  state.news_reader_has_nav = true;
  state.news_reader_can_prev = false;
  state.news_reader_can_next = true;
  EXPECT_EQ(ViewCatalog::KeyForLayout("news_reader", true), "news_reader_wide");
  EXPECT_EQ(ViewCatalog::KeyForLayout("news_reader", false), "news_reader");
  const std::string phone = ViewCatalog::LoadBody("news_reader");
  EXPECT_NE(phone.find("sheet-header"), std::string::npos) << "the phone's sheet view as it was";
  const std::string wide = ViewCatalog::LoadBody(ViewCatalog::KeyForLayout("news_reader", true));
  for (const char* binding :
       {"news_reader_prev()", "news_reader_next()", "news_reader_open_original()", "news_reader_translated",
        "news_reader_loading", "news_reader_failed", "news_reader_body_rml", "news_reader_has_nav",
        "news_reader_can_prev", "news_reader_can_next"}) {
    EXPECT_NE(wide.find(binding), std::string::npos) << binding;
  }
  for (const int window : {768, 1280}) {
    SCOPED_TRACE(window);
    const std::string rml =
        "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
        Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") +
        "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
        "<div class=\"shell-body shell-body--wide\" style=\"position: absolute; top: 0; left: 0; width: " +
        std::to_string(window) +
        "dp; height: 700dp;\"><div class=\"shell-layer shell-layer-overlay\" style=\"position: absolute; top: 0; left: "
        "0; right: 0; bottom: 0;\"><div class=\"shell-scrim\"></div>"
        "<div id=\"frame\" class=\"shell-frame shell-frame--news_reader\"><div class=\"shell-overlay-body\">" +
        wide + "</div></div></div></div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    context->Update();
    const Box frame = BoxOf(document->GetElementById("frame"));
    EXPECT_LE(frame.W(), 720.f);
    EXPECT_NEAR(static_cast<double>((frame.left + frame.right) / 2.f), window / 2.0, 1.0) << "centred";
    EXPECT_GE(frame.left, 0.f);
    EXPECT_LE(frame.right, static_cast<float>(window));
    ui::Element* title = First("news-reader-title");
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->GetInnerRML(), "Chip makers brace for new export rules");
    ui::Element* scroll = First("news-reader-scroll--wide");
    ASSERT_NE(scroll, nullptr);
    EXPECT_GE(BoxOf(title).top, frame.top);
    EXPECT_LE(BoxOf(title).right, frame.right - 36.f + 0.5f) << "clear of the close button";
    ui::Element* translated = First("news-reader-translated");
    ASSERT_NE(translated, nullptr);
    EXPECT_TRUE(translated->IsVisible()) << "a translated digest says so";
    EXPECT_GT(BoxOf(translated).H(), 10.f);
    EXPECT_GE(BoxOf(translated).top, BoxOf(title).bottom - 6.5f) << "under the title";
    EXPECT_LE(BoxOf(translated).bottom, BoxOf(First("news-reader-body")).top + 0.5f) << "above the digest";
    ui::Element* bar = First("news-reader-bar--wide");
    ASSERT_NE(bar, nullptr);
    EXPECT_GE(BoxOf(bar).top, BoxOf(scroll).bottom - 0.5f) << "the bar under the scrolling body";
    EXPECT_LE(BoxOf(bar).bottom, frame.bottom + 0.5f);
    const ui::ElementList open = All("news-reader-open");
    const ui::ElementList steps = All("news-reader-step");
    ASSERT_EQ(open.size(), 1u);
    ASSERT_EQ(steps.size(), 2u);
    EXPECT_LT(BoxOf(steps[0]).left, BoxOf(open[0]).left) << "Previous, Open original, Next";
    EXPECT_GT(BoxOf(steps[1]).left, BoxOf(open[0]).left);
    for (ui::Element* button : {open[0], steps[0], steps[1]}) {
      EXPECT_GE(BoxOf(button).H(), 44.f);
      EXPECT_LE(BoxOf(button).right, frame.right);
    }
    EXPECT_EQ(All("sheet-header").size(), 0u);
    EXPECT_EQ(First("news-reader-spin")->GetBox().GetSize(ui::BoxArea::Border).y, 0.f) << "no spinner when loaded";
    document->Close();
    context->Update();
  }
  // Loading: the spinner and line; failed: the fixed line.
  state.news_reader_loading = true;
  state.news_reader_body_rml = "";
  DataModelHost::Instance().Dirty("chat", "news_reader_loading");
  DataModelHost::Instance().Dirty("chat", "news_reader_body_rml");
  const std::string rml =
      "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
      Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") +
      "</style></head><body><div class=\"shell-body shell-body--wide\" style=\"width: 900dp; height: 700dp;\">" + wide +
      "</div></body></rml>";
  document = context->LoadDocumentFromMemory(rml);
  ASSERT_NE(document, nullptr);
  document->Show();
  context->Update();
  EXPECT_GT(BoxOf(First("news-reader-spin")).H(), 0.f);
  // The digest grows as it is written (ChatController redraws the same binding): the panel shows each longer text.
  state.news_reader_loading = false;
  DataModelHost::Instance().Dirty("chat", "news_reader_loading");
  for (const char* so_far : {"The first", "The first words, then more of them."}) {
    state.news_reader_body_rml = std::string("<p>") + so_far + "</p>";
    DataModelHost::Instance().Dirty("chat", "news_reader_body_rml");
    context->Update();
    context->Update();
    EXPECT_NE(std::string(First("news-reader-body")->GetInnerRML().c_str()).find(so_far), std::string::npos) << so_far;
  }
  state.news_reader_translated = false;
  DataModelHost::Instance().Dirty("chat", "news_reader_translated");
  context->Update();
  EXPECT_FALSE(First("news-reader-translated")->IsVisible()) << "an untranslated digest: no line";
  document->Close();
}

// The failed read on the wide panel: "Try again" directly under the fixed sentence, above the bottom row, at least 44dp
// high, only while news_reader_can_retry is set; the row (Open original, Previous / Next) stays where it is.
TEST_F(ChatWideLayoutTest, TheWideNewsReaderOffersTryAgainOnlyInTheFailedState) {
  state.news_reader_title = "Chip makers brace for new export rules";
  state.news_reader_has_nav = true;
  state.news_reader_can_prev = true;
  state.news_reader_can_next = true;
  state.news_reader_body_rml = "";
  state.news_reader_failed = true;
  state.news_reader_can_retry = true;
  const std::string wide = ViewCatalog::LoadBody(ViewCatalog::KeyForLayout("news_reader", true));
  EXPECT_NE(wide.find("news_reader_retry()"), std::string::npos);
  EXPECT_NE(wide.find("news_reader_can_retry"), std::string::npos);
  EXPECT_NE(Asset("views/news_reader_wide.rml").find("{{i18n:chat.news.retry}}"), std::string::npos);
  for (const bool failed : {true, false}) {
    SCOPED_TRACE(failed);
    state.news_reader_failed = failed;
    state.news_reader_can_retry = failed;
    DataModelHost::Instance().Dirty("chat", "news_reader_failed");
    DataModelHost::Instance().Dirty("chat", "news_reader_can_retry");
    const std::string rml =
        "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
        Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") +
        "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
        "<div class=\"shell-body shell-body--wide\" style=\"position: absolute; top: 0; left: 0; width: 1280dp; "
        "height: 700dp;\"><div class=\"shell-layer shell-layer-overlay\" style=\"position: absolute; top: 0; left: "
        "0; right: 0; bottom: 0;\"><div class=\"shell-scrim\"></div>"
        "<div id=\"frame\" class=\"shell-frame shell-frame--news_reader\"><div class=\"shell-overlay-body\">" +
        wide + "</div></div></div></div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    context->Update();
    const Box frame = BoxOf(document->GetElementById("frame"));
    ui::Element* retry = First("news-reader-retry");
    ASSERT_NE(retry, nullptr);
    if (!failed) {
      EXPECT_FALSE(retry->IsVisible()) << "no button without the failure";
    } else {
      ASSERT_TRUE(retry->IsVisible());
      const Box r = BoxOf(retry);
      ui::Element* note = nullptr;
      for (ui::Element* n : All("news-reader-note")) {
        if (n->IsVisible() && BoxOf(n).H() > 0.f) { // the hidden loading line has no box
          note = n;
        }
      }
      ASSERT_NE(note, nullptr);
      const Box n = BoxOf(note);
      const Box bar = BoxOf(First("news-reader-bar--wide"));
      EXPECT_GE(r.top, n.bottom - 0.5f) << "under the sentence";
      EXPECT_LE(r.top - n.bottom, 30.f) << "directly under it";
      EXPECT_LE(r.bottom, bar.top + 0.5f) << "above the bottom row";
      EXPECT_GE(r.H(), 44.f);
      EXPECT_LE(r.H(), 46.f) << "the desktop size, not the phone's 50dp";
      EXPECT_GE(r.W(), 120.f);
      EXPECT_GE(r.left, frame.left);
      EXPECT_LE(r.right, frame.right);
      EXPECT_EQ(All("news-reader-open").size(), 1u);
      EXPECT_EQ(All("news-reader-step").size(), 2u);
      EXPECT_GE(BoxOf(First("news-reader-open")).H(), 44.f);
    }
    document->Close();
    context->Update();
  }
}

TEST_F(ChatWideLayoutTest, ANewsLinkHoversOnTheWideLayoutOnly) {
  for (const char* theme : {"light", "dark"}) {
    const std::string colors = Asset((std::string("themes/colors-") + theme + ".rcss").c_str());
    EXPECT_NE(colors.find(".shell-body--wide .chat-link.news-link:hover"), std::string::npos) << theme;
    EXPECT_NE(colors.find(".shell-body--wide .bubble-user .chat-link.news-link:hover"), std::string::npos) << theme;
    EXPECT_NE(colors.find(".chat-link.news-read"), std::string::npos) << "read items stay grey (the phone's rule)";
  }
}

} // namespace
} // namespace pbr
