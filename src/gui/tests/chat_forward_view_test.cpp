// The phone chat while choosing messages to forward (DEV/PLAN-message-forward-text.md, step 4): the real chat.rml and
// composer.rml on the real stylesheets, mounted the way the compact chat overlay lays them out, with rows in the mode.
// The menu's entry is read from ChatMenus.cpp (ForRequest needs a live messaging facade, as in the other menu tests).
#include "gui/chat/ChatController.h"
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/TranscriptRml.h"
#include "lib/ai/MarkdownToRml.h"
#include "gui/shell/DataModelHost.h"

#include "feature/presenters/MessageForward.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/delivery/ChatPayloadCodec.h"

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

std::string Asset(const char* relative) {
  return ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative);
}

constexpr int kWidth = 390;
constexpr int kHeight = 844;

MessageDisplayRow Row(const std::string& id, const std::string& content_rml, const bool ok, const bool ticked) {
  MessageDisplayRow row;
  row.message_id = id.c_str();
  row.content_rml = content_rml.c_str();
  row.row_class = "message-row-peer";
  row.forward_mode = true;
  row.forward_ok = ok;
  row.forward_ticked = ticked;
  return row;
}

class ChatForwardViewTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext("chat-forward-view-test", {kWidth, kHeight});
    ASSERT_NE(context, nullptr);
    ASSERT_TRUE(DataModelHost::Instance().Register(context, "chat", [this](ui::DataModelConstructor& ctor) {
      BindChatScreenState(ctor, state);
      ctor.BindEventCallback("forward_tap", [this](ui::DataModelHandle, ui::Event&, const ui::VariantList& args) {
        if (!args.empty()) {
          taps.push_back(args[0].Get<ui::String>().c_str());
        }
      });
      ctor.BindEventCallback("forward_cancel",
                             [this](ui::DataModelHandle, ui::Event&, const ui::VariantList&) { ++cancels; });
      ctor.BindEventCallback("forward_next",
                             [this](ui::DataModelHandle, ui::Event&, const ui::VariantList&) { ++nexts; });
      ctor.BindEventCallback("open_message_link",
                             [this](ui::DataModelHandle, ui::Event&, const ui::VariantList&) { ++link_opens; });
    }));
    state.use_messages_layout = true;
    state.forward_selecting = true;
    state.forward_count_label = "2 selected";
    state.forward_can_next = true;
    const std::string link =
        "<div class=\"bubble bubble-assistant\"><span class=\"chat-link\" id=\"the-link\" style=\"display: "
        "inline-block; width: 140dp; height: 28dp;\" "
        "data-event-click=\"open_message_link('m1', 0)\">https://example.test</span></div>";
    const std::string phone =
        "<div class=\"bubble bubble-assistant\"><span class=\"chat-link\" id=\"the-phone\" style=\"display: "
        "inline-block; width: 140dp; height: 28dp;\" "
        "data-event-click=\"open_message_link('m2', 0)\">+1 555 0100</span></div>";
    const std::string card =
        "<div class=\"chat-card chat-contact-card\" id=\"the-card\" style=\"height: 48dp;\"><h3 "
        "class=\"heading-3\">Mia</h3></div>";
    state.messages = {Row("m1", link, true, true), Row("m2", phone, true, false), Row("m3", card, true, true),
                      Row("m4", "<div class=\"bubble bubble-assistant\">a picture</div>", false, false)};
    const LocalizationService& i18n = LocalizationService::Instance();
    const std::string rml = "<rml><head><style>" + Asset("themes/components.rcss") + Asset("themes/colors-light.rcss") +
                            "</style></head><body style=\"padding: 0;\">"
                            "<div style=\"position: absolute; top: 0; left: 0; width: " +
                            std::to_string(kWidth) + "dp; height: " + std::to_string(kHeight) +
                            "dp;\">"
                            "<div class=\"shell-layer shell-layer-base shell-layer-compact\">"
                            "<div class=\"shell-chat-overlay\" id=\"shell-chat-overlay\">"
                            "<div class=\"shell-chat-overlay-chrome row surface-chrome surface-chrome--frost\">"
                            "<div class=\"bar-material bar-material--frost\"></div><div class=\"bar-fade\"></div></div>"
                            "<div class=\"shell-pane-body\" id=\"pane-body-chat\">" +
                            i18n.LocalizeText(Asset("views/chat.rml")) +
                            "</div>"
                            "<div class=\"shell-composer-bar\">"
                            "<div class=\"shell-composer-mount surface-chrome\" id=\"shell-composer-mount\">" +
                            i18n.LocalizeText(Asset("views/composer.rml")) +
                            "</div></div>"
                            "</div></div></div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    context->Update();
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext("chat-forward-view-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }
  void Refresh() {
    for (const char* field : {"messages", "forward_selecting", "forward_count_label", "forward_can_next"}) {
      DataModelHost::Instance().Dirty("chat", field);
    }
    context->Update();
    context->Update();
  }
  std::vector<ui::Element*> Shown(const char* cls) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    std::vector<ui::Element*> shown;
    for (ui::Element* element : found) {
      if (element->IsVisible(/*include_ancestors=*/true)) {
        shown.push_back(element);
      }
    }
    return shown;
  }
  struct Edges {
    float left, top, right, bottom;
  };
  static Edges BorderEdges(ui::Element* element) {
    const ui::Vector2f at = element->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f size = element->GetBox().GetSize(ui::BoxArea::Border);
    return {at.x, at.y, at.x + size.x, at.y + size.y};
  }
  void Tap(ui::Element* element) {
    const Edges e = BorderEdges(element);
    context->ProcessMouseMove(static_cast<int>((e.left + e.right) / 2), static_cast<int>((e.top + e.bottom) / 2), 0);
    context->ProcessMouseButtonDown(0, 0);
    context->ProcessMouseButtonUp(0, 0);
    context->Update();
  }
  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  ChatScreenState state;
  std::vector<std::string> taps;
  int cancels = 0;
  int nexts = 0;
  int link_opens = 0;
};

TEST_F(ChatForwardViewTest, SelectionModeShowsCheckboxesOnlyOnForwardableRows) {
  EXPECT_EQ(Shown("message-forward-check").size(), 3u) << "three of the four rows can go";
  const std::vector<ui::Element*> ticked = Shown("message-forward-check--on");
  EXPECT_EQ(ticked.size(), 2u) << "m1 and m3 are ticked";
  EXPECT_EQ(Shown("message-forward-cover").size(), 4u) << "every row takes a tap, the picture's too";
}

TEST_F(ChatForwardViewTest, InSelectionModeALinkOrPhoneNumberOrCardTapOnlyToggles) {
  Tap(document->GetElementById("the-link"));
  Tap(document->GetElementById("the-phone"));
  Tap(document->GetElementById("the-card"));
  EXPECT_EQ(link_opens, 0) << "no browser, no dial";
  EXPECT_EQ(taps, (std::vector<std::string>{"m1", "m2", "m3"})) << "each tap ticked its own row";

  // The bubbles as the transcript really draws them: the last one of a run carries a tail host, a bubble with reactions
  // a reaction host, an assistant answer a list; all of them sit later in the markup than the cover and must not
  // take the tap from it.
  taps.clear();
  TranscriptRow first;
  first.message.id = "r1";
  first.message.sender_contact_id = "account:a";
  first.message.content_type = ChatContentType::Text;
  first.message.text = "first of a run";
  first.message.timestamp = 1700049600000;
  TranscriptRow last = first; // the last of the run: its bubble has the tail
  last.message.id = "r2";
  last.message.text = "last of a run, see https://example.test/last";
  last.message.timestamp += 1000;
  TranscriptRow reacted = first;
  reacted.message.id = "r3";
  reacted.message.sender_contact_id = "account:b";
  reacted.message.text = "with reactions https://example.test/reacted";
  reacted.message.timestamp += 5 * 60 * 1000;
  reacted.reactions.push_back(TranscriptReaction{"\xF0\x9F\x91\x8D", 2, true, {}});
  TranscriptRow answer = first;
  answer.message.id = "r4";
  answer.message.sender_contact_id = kAiAssistantContactId;
  answer.message.text = "1. one https://example.test/one\n2. two";
  answer.message.content_rml = MarkdownToRml(answer.message.text).rml;
  answer.message.timestamp += 10 * 60 * 1000;
  TranscriptFacts facts;
  facts.sender_label = [](const std::string& id) { return id; };
  facts.sender_stable_id = [](const std::string& id) { return id; };
  state.messages = RenderTranscript({first, last, reacted, answer}, facts);
  for (MessageDisplayRow& row : state.messages) {
    row.forward_mode = true;
    row.forward_ok = true;
  }
  Refresh();
  ASSERT_NE(std::string(state.messages[1].content_rml.c_str()).find("bubble-tail-host"), std::string::npos)
      << "the fixture really has a tail";
  ASSERT_NE(std::string(state.messages[2].content_rml.c_str()).find("chat-reaction-host"), std::string::npos);
  ASSERT_NE(std::string(state.messages[3].content_rml.c_str()).find("<li"), std::string::npos);
  size_t tapped = 0;
  for (const char* cls : {"bubble", "chat-link", "chat-reaction-host", "bubble-tail-host"}) {
    for (ui::Element* element : Shown(cls)) {
      const size_t before = taps.size();
      Tap(element);
      ASSERT_EQ(taps.size(), before + 1) << cls << ": a tap on it must tick its row";
      ++tapped;
    }
  }
  ui::ElementList items;
  document->GetElementsByTagName(items, "li");
  for (ui::Element* element : items) {
    const size_t before = taps.size();
    Tap(element);
    ASSERT_EQ(taps.size(), before + 1) << "a list item";
    ++tapped;
  }
  EXPECT_GE(tapped, 8u);
  EXPECT_EQ(link_opens, 0) << "no link opened in selection mode";
}

TEST_F(ChatForwardViewTest, OutsideSelectionALinkStillOpens) {
  for (MessageDisplayRow& row : state.messages) {
    row.forward_mode = false;
    row.forward_ok = false;
    row.forward_ticked = false;
  }
  state.forward_selecting = false;
  Refresh();
  EXPECT_TRUE(Shown("message-forward-cover").empty());
  ui::Element* link = document->GetElementById("the-link");
  ASSERT_NE(link, nullptr);
  Tap(link);
  EXPECT_EQ(link_opens, 1);
  EXPECT_TRUE(taps.empty());
}

TEST_F(ChatForwardViewTest, ARowThatCannotBeForwardedStillTakesTheTap) {
  // The row has no tick box, and still takes the tap (its cover): the flow answers it with the pill (ForwardFlowTest).
  const std::vector<ui::Element*> covers = Shown("message-forward-cover");
  ASSERT_EQ(covers.size(), 4u);
  Tap(covers.back());
  EXPECT_EQ(taps, (std::vector<std::string>{"m4"}));
}

TEST_F(ChatForwardViewTest, TheTitleBarCountsAndCancelsAndTheComposerGivesWayToTheForwardBar) {
  ASSERT_EQ(Shown("chat-forward-title").size(), 1u);
  EXPECT_EQ(std::string(Shown("chat-forward-title")[0]->GetInnerRML().c_str()), "2 selected");
  EXPECT_TRUE(Shown("chat-header-title").empty()) << "the chat's title bar is replaced";
  EXPECT_TRUE(Shown("prompt-composer").empty()) << "the composer is hidden";
  Tap(Shown("chat-forward-cancel").at(0));
  EXPECT_EQ(cancels, 1);
  Tap(Shown("chat-forward-next").at(0));
  EXPECT_EQ(nexts, 1);
}

TEST_F(ChatForwardViewTest, TheForwardBarIsDisabledWithNothingSelected) {
  state.forward_can_next = false;
  state.forward_count_label = "0 selected";
  Refresh();
  ui::Element* next = Shown("chat-forward-next").at(0);
  EXPECT_TRUE(next->HasAttribute("disabled"));
  // (The engine still delivers a tap on a disabled button; the flow ignores it: ForwardFlowTest.TheBottomButtonIsOff...)
  state.forward_can_next = true;
  Refresh();
  EXPECT_FALSE(Shown("chat-forward-next").at(0)->HasAttribute("disabled"));
}

TEST_F(ChatForwardViewTest, TheTapTargetsAreAtLeast44dpAndTheFormerComposerIsBack) {
  for (const char* cls : {"chat-forward-cancel", "chat-forward-next"}) {
    const Edges e = BorderEdges(Shown(cls).at(0));
    EXPECT_GE(e.bottom - e.top, 44.f) << cls;
    EXPECT_GE(e.right - e.left, 44.f) << cls;
  }
  state.forward_selecting = false;
  Refresh();
  EXPECT_EQ(Shown("prompt-composer").size(), 1u) << "leaving the mode brings the composer back";
  EXPECT_TRUE(Shown("chat-forward-next").empty());
}

ThreadMessage TextMessage(const std::string& text) {
  ThreadMessage row;
  row.id = "m";
  row.content_type = ChatContentType::Text;
  row.text = text;
  return row;
}

TEST(ForwardPreviewTest, ThePreviewShowsTwoLinesOfTextACardNameOrACount) {
  const auto count = [](const size_t n) { return std::to_string(n) + " messages"; };
  EXPECT_EQ(ForwardPreviewText({TextMessage("one\ntwo\nthree\nfour")}, count), "one\ntwo");
  EXPECT_EQ(ForwardPreviewText({TextMessage("only line")}, count), "only line");
  EXPECT_EQ(ForwardPreviewText({TextMessage("first\n")}, count), "first\n");

  ThreadMessage card = TextMessage("Mia");
  card.content_type = ChatContentType::ContactCard;
  ChatContactCardFields fields;
  fields.display_name = "Mia Wong";
  fields.card_json = R"({"account_id":"acct-mia","v":1})";
  card.payload_json = ChatPayloadCodec::ContactCardFieldsToJson(fields);
  EXPECT_EQ(ForwardPreviewText({card}, count), "Mia Wong");

  EXPECT_EQ(ForwardPreviewText({TextMessage("a"), TextMessage("b"), card}, count), "3 messages");
  EXPECT_EQ(ForwardPreviewText({}, count), "");
}

// The menu: "Forward" right after Reply and before Copy, for what ChatPresenter::CanForward says yes to (texts and
// cards; a picture is no, and the presenter's own tests say so). A press while choosing offers no menu.
TEST(ChatForwardMenuTest, TheMessageMenuOffersForwardAfterReplyForTextAndCards) {
  const std::string source = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatMenus.cpp");
  const size_t for_request = source.find("std::vector<ContextMenuAction> ChatMenus::ForRequest(");
  ASSERT_NE(for_request, std::string::npos);
  const size_t reply = source.find("\"reply_message\"", for_request);
  const size_t forward = source.find("\"forward_message\"", for_request);
  const size_t copy = source.find("\"copy_message\"", for_request);
  ASSERT_NE(reply, std::string::npos);
  ASSERT_NE(forward, std::string::npos);
  ASSERT_NE(copy, std::string::npos);
  EXPECT_LT(reply, forward) << "after Reply";
  EXPECT_LT(forward, copy) << "before Copy";
  EXPECT_NE(source.find("Tr(\"chat.menu.forward\")", for_request), std::string::npos);
  EXPECT_NE(source.find("host_.BeginForward(message_id)", for_request), std::string::npos);
}

TEST(ChatForwardMenuTest, TheMessageMenuHasNoForwardForAPicture) {
  // The item is added only when the presenter says the message can go; a picture is an Attachment, which it refuses.
  const std::string source = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatMenus.cpp");
  const size_t forward = source.find("\"forward_message\"");
  ASSERT_NE(forward, std::string::npos);
  const std::string before = source.substr(source.rfind("if (", forward), forward - source.rfind("if (", forward));
  EXPECT_NE(before.find("presenter_.CanForward(message_id)"), std::string::npos);
  ThreadMessage picture = TextMessage("");
  picture.content_type = ChatContentType::Attachment;
  EXPECT_FALSE(CanForwardMessage(picture));
  EXPECT_TRUE(CanForwardMessage(TextMessage("words")));
}

TEST(ChatForwardMenuTest, ChoosingMessagesOffersNoMenuOnAPress) {
  const std::string source = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatMenus.cpp");
  const size_t for_request = source.find("std::vector<ContextMenuAction> ChatMenus::ForRequest(");
  const size_t choosing = source.find("if (presenter_.ForwardChoosing()) {", for_request);
  const size_t message_id = source.find("FindAttributeUp(request.target, \"message-id\")", for_request);
  ASSERT_NE(choosing, std::string::npos);
  EXPECT_LT(choosing, message_id) << "decided before the message's own menu is built";
}

} // namespace
} // namespace pbr
