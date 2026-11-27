// The phone chat while choosing messages to forward (DEV/PLAN-message-forward-text.md, step 4): the real chat.rml and
// composer.rml on the real stylesheets, mounted the way the compact chat overlay lays them out, with rows in the mode.
// The menu's entry is read from ChatMenus.cpp (ForRequest needs a live messaging facade, as in the other menu tests).
#include "gui/chat/ChatController.h"
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/TranscriptRml.h"
#include "lib/ai/MarkdownToRml.h"
#include "gui/shell/DataModelHost.h"

#include "feature/presenters/ChatPresenter.h"
#include "feature/presenters/MessageForward.h"
#include "lib/messaging/TranscriptAssembly.h"
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
      // What an attachment bubble does outside selection mode: none of it may happen while choosing.
      for (const char* name :
           {"open_attachment", "open_picture", "download_attachment", "retry_attachment", "play_voice",
            "play_audio_file", "audio_seek_press", "open_sender", "open_reactors", "jump_to_quote"}) {
        ctor.BindEventCallback(name, [this, name](ui::DataModelHandle, ui::Event&, const ui::VariantList&) {
          attachment_events.push_back(name);
        });
      }
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
  std::vector<std::string> attachment_events;
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

/** A picture, a file and a voice message drawn by the real transcript, the way a chat shows them. */
std::vector<MessageDisplayRow> AttachmentTranscript() {
  const auto row = [](const std::string& id, const std::string& name, const std::string& mime, const uint64_t bytes,
                      const int64_t at) {
    ChatAttachmentFields fields;
    fields.mime = mime;
    fields.filename = name;
    fields.byte_length = bytes;
    fields.url = "https://example.invalid/blob/" + id;
    fields.content_hash.assign(32, static_cast<uint8_t>(at));
    fields.content_key.assign(32, 2);
    fields.blob_nonce.assign(24, 3);
    TranscriptRow entry;
    entry.message.id = id;
    entry.message.thread_id = "t";
    entry.message.sender_contact_id = "account:b";
    entry.message.content_type = ChatContentType::Attachment;
    entry.message.text = name;
    entry.message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
    entry.message.timestamp = 1700049600000 + at * 60'000;
    entry.message.delivery = MessageDelivery::Relayed;
    return entry;
  };
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
  return RenderTranscript(
      {row("a1", "photo.jpg", "image/jpeg", 900'000, 1), row("a2", "report.pdf", "application/pdf", 120'000, 2),
       row("a3", "voice-4200.ogg", "audio/ogg", 25'000, 3)},
      facts);
}

TEST_F(ChatForwardViewTest, AttachmentRowsHaveCheckboxesInSelectionMode) {
  state.messages = AttachmentTranscript();
  ASSERT_EQ(state.messages.size(), 3u);
  for (MessageDisplayRow& row : state.messages) {
    EXPECT_TRUE(row.forwardable) << "a picture, a file and a voice message can be chosen (CanForwardMessage)";
    row.forward_mode = true;
    row.forward_ok = row.forwardable;
    row.forward_ticked = false;
  }
  state.messages[1].forward_ticked = true;
  Refresh();
  EXPECT_EQ(Shown("message-forward-check").size(), 3u) << "every attachment row shows its tick box";
  EXPECT_EQ(Shown("message-forward-check--on").size(), 1u);
  EXPECT_EQ(Shown("message-forward-cover").size(), 3u);
}

TEST_F(ChatForwardViewTest, ATapOnAnAttachmentRowInSelectionModeOnlyTicksItsRow) {
  // The bubbles as the transcript really draws them: a picture's clip, a file card and a voice bubble each carry their
  // own click (open / download / play). While choosing, the row's cover is above all of them and takes the tap.
  state.messages = AttachmentTranscript();
  for (MessageDisplayRow& row : state.messages) {
    row.forward_mode = true;
    row.forward_ok = true;
  }
  Refresh();
  const std::string markup = std::string(state.messages[0].content_rml.c_str()) +
                             state.messages[1].content_rml.c_str() + state.messages[2].content_rml.c_str();
  ASSERT_NE(markup.find("open_picture"), std::string::npos) << "the picture's clip has its own click";
  ASSERT_NE(markup.find("play_voice"), std::string::npos) << "the voice bubble has its own click";
  size_t tapped = 0;
  for (const char* tag : {"div", "span", "button", "p", "img", "svg"}) {
    ui::ElementList found;
    document->GetElementsByTagName(found, tag);
    for (ui::Element* element : found) {
      if (!element->HasAttribute("data-event-click") || !element->IsVisible(true)) {
        continue;
      }
      bool in_rows = false; // the bars and buttons around the transcript are not rows
      for (ui::Element* up = element->GetParentNode(); up; up = up->GetParentNode()) {
        in_rows = in_rows || up->GetId() == "chat-messages";
      }
      if (!in_rows) {
        continue;
      }
      if (element->GetAttribute<ui::String>("data-event-click", "").find("forward_tap") != ui::String::npos) {
        continue; // the cover itself
      }
      const size_t before = taps.size();
      Tap(element);
      ASSERT_EQ(taps.size(), before + 1) << "a tap on "
                                         << element->GetAttribute<ui::String>("data-event-click", "").c_str()
                                         << " must tick its row";
      ++tapped;
    }
  }
  EXPECT_GE(tapped, 2u) << "at least the picture and the voice bubble were tapped";
  EXPECT_TRUE(attachment_events.empty()) << "nothing was opened, downloaded or played: " << attachment_events.size();
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

// A click on the Forward button never ticks the row that lies under the bar (the transcript runs under the composer
// bar): the rows' covers and ticks sit below the bar in the stacking order.
TEST_F(ChatForwardViewTest, AClickOnTheForwardButtonDoesNotTickTheRowUnderIt) {
  state.messages.clear();
  for (int i = 0; i < 40; ++i) {
    state.messages.push_back(
        Row("r" + std::to_string(i), "<div class=\"bubble bubble-assistant\">a message</div>", true, false));
  }
  Refresh();
  ui::Element* messages = document->GetElementById("chat-messages");
  ASSERT_NE(messages, nullptr);
  messages->SetScrollTop(messages->GetScrollHeight() / 2.f);
  context->Update();
  ui::Element* next = Shown("chat-forward-next").at(0);
  const Edges e = BorderEdges(next);
  const float cx = (e.left + e.right) / 2, cy = (e.top + e.bottom) / 2;
  bool row_under = false;
  for (ui::Element* cover : Shown("message-forward-cover")) {
    const Edges c = BorderEdges(cover);
    row_under = row_under || (c.left <= cx && cx <= c.right && c.top <= cy && cy <= c.bottom);
  }
  ASSERT_TRUE(row_under) << "precondition: a row lies under the button";
  Tap(next);
  EXPECT_EQ(nexts, 1);
  EXPECT_TRUE(taps.empty()) << "the click also ticked the row under the bar";
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

// The menu: "Forward" (this one message) and "Select" (multi-select) right after Reply and before Copy, for what
// ChatPresenter::CanForward says yes to (texts and cards; a picture is no, and the presenter's own tests say so). A
// press while choosing offers no menu.
TEST(ChatForwardMenuTest, TheMessageMenuOffersForwardThenSelectAfterReply) {
  const std::string source = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatMenus.cpp");
  const size_t for_request = source.find("std::vector<ContextMenuAction> ChatMenus::ForRequest(");
  ASSERT_NE(for_request, std::string::npos);
  const size_t reply = source.find("\"reply_message\"", for_request);
  const size_t forward = source.find("\"forward_message\"", for_request);
  const size_t select = source.find("\"select_messages\"", for_request);
  const size_t copy = source.find("\"copy_message\"", for_request);
  ASSERT_NE(reply, std::string::npos);
  ASSERT_NE(forward, std::string::npos);
  ASSERT_NE(select, std::string::npos);
  ASSERT_NE(copy, std::string::npos);
  EXPECT_LT(reply, forward) << "after Reply";
  EXPECT_LT(forward, select) << "Select right after Forward";
  EXPECT_LT(select, copy) << "before Copy";
  EXPECT_NE(source.find("Tr(\"chat.menu.forward\")", for_request), std::string::npos);
  EXPECT_NE(source.find("Tr(\"chat.menu.select\")", for_request), std::string::npos);
  EXPECT_NE(source.find("host_.BeginForward(message_id)", for_request), std::string::npos);
  EXPECT_NE(source.find("host_.BeginSelect(message_id)", for_request), std::string::npos);
}

/** The `{` nesting depth of `source` at `at`, counting from the start of the string. */
int BraceDepthAt(const std::string& source, const size_t at) {
  int depth = 0;
  for (size_t i = 0; i < at && i < source.size(); ++i) {
    depth += source[i] == '{' ? 1 : source[i] == '}' ? -1 : 0;
  }
  return depth;
}

ThreadMessage AttachmentMessage(const std::string& name, const std::string& mime, const uint64_t bytes) {
  ChatAttachmentFields fields;
  fields.mime = mime;
  fields.filename = name;
  fields.byte_length = bytes;
  fields.url = "https://example.invalid/blob";
  fields.content_hash.assign(32, 1);
  fields.content_key.assign(32, 2);
  fields.blob_nonce.assign(24, 3);
  ThreadMessage row = TextMessage(name);
  row.content_type = ChatContentType::Attachment;
  row.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
  row.sender_contact_id = "contact-a";
  row.delivery = MessageDelivery::Relayed;
  return row;
}

TEST(ChatForwardMenuTest, TheMessageMenuOffersForwardForAPicture) {
  // The two items hang on ChatPresenter::CanForward alone, not on the message having words (a picture's "text" is its
  // file name, a voice message has none): they sit at the menu's top level, outside the `if (!text.empty())` of Reply.
  const std::string source = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatMenus.cpp");
  const size_t for_request = source.find("std::vector<ContextMenuAction> ChatMenus::ForRequest(");
  ASSERT_NE(for_request, std::string::npos);
  const size_t reply = source.find("\"reply_message\"", for_request);
  const size_t forward = source.find("\"forward_message\"", for_request);
  const size_t select = source.find("\"select_messages\"", for_request);
  ASSERT_NE(reply, std::string::npos);
  ASSERT_NE(forward, std::string::npos);
  ASSERT_NE(select, std::string::npos);
  EXPECT_NE(source.substr(reply, forward - reply).find("}\n  // Forward"), std::string::npos)
      << "Reply's block is closed before Forward starts";
  EXPECT_EQ(BraceDepthAt(source, forward), BraceDepthAt(source, reply))
      << "Forward is a sibling of Reply's block, not nested in it (it was one level deeper when it needed words)";
  EXPECT_EQ(BraceDepthAt(source, select), BraceDepthAt(source, forward)) << "Select in the same block as Forward";
  // The item is under `if (slots.forward)`, and the slots are decided with the presenter's answer, not with the words.
  const size_t guard = source.rfind("if (", forward);
  EXPECT_EQ(source.substr(guard, source.find(')', guard) - guard), "if (slots.forward")
      << "one check covers both items";
  const size_t decide = source.find("DecideMessageMenuSlots(");
  ASSERT_NE(decide, std::string::npos);
  EXPECT_NE(source.find("presenter_.CanForward(message_id)", decide), std::string::npos);
  // What CanForward says yes to now.
  for (const ThreadMessage& row : {AttachmentMessage("photo.jpg", "image/jpeg", 2'000'000),
                                   AttachmentMessage("clip.mov", "video/quicktime", 9'000'000),
                                   AttachmentMessage("report.pdf", "application/pdf", 90'000),
                                   AttachmentMessage("voice-4200.ogg", "audio/ogg", 25'000)}) {
    EXPECT_TRUE(CanForwardMessage(row)) << row.text;
  }
  EXPECT_TRUE(CanForwardMessage(TextMessage("words")));
}

TEST(ChatForwardMenuTest, APictureOrVideoMessageHasNoCopyItem) {
  // Copy gave only the file name: gone from every attachment message (pictures, videos, files, voice messages).
  const std::string source = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatMenus.cpp");
  const size_t for_request = source.find("std::vector<ContextMenuAction> ChatMenus::ForRequest(");
  const size_t copy = source.find("\"copy_message\"", for_request);
  ASSERT_NE(copy, std::string::npos);
  const size_t guard = source.rfind("if (", copy);
  EXPECT_EQ(source.substr(guard, source.find('{', guard) - guard), "if (!presenter_.IsAttachmentMessage(message_id)) ")
      << "Copy is offered only for a message that is not an attachment";
  EXPECT_EQ(source.find("\"copy_message\"", copy + 1), std::string::npos) << "the only place the item is made";

  // The presenter's answer, for each kind and for text and a missing row.
  ChatPresenter presenter;
  std::map<std::string, ThreadMessage> rows;
  for (const ThreadMessage& row : {AttachmentMessage("photo.jpg", "image/jpeg", 2'000'000),
                                   AttachmentMessage("clip.mov", "video/quicktime", 9'000'000),
                                   AttachmentMessage("report.pdf", "application/pdf", 90'000),
                                   AttachmentMessage("voice-4200.ogg", "audio/ogg", 25'000)}) {
    ThreadMessage stored = row;
    stored.id = row.text;
    rows[stored.id] = stored;
  }
  rows["words"] = TextMessage("words");
  rows["words"].id = "words";
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

TEST(ChatForwardMenuTest, ATextMessageStillHasCopy) {
  const std::string source = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatMenus.cpp");
  const size_t for_request = source.find("std::vector<ContextMenuAction> ChatMenus::ForRequest(");
  const size_t copy = source.find("\"copy_message\"", for_request);
  ASSERT_NE(copy, std::string::npos);
  // Still made for any message with words that is not an attachment, with the same action (the text, to the clipboard),
  // and before Ask AI: unchanged for text and contact cards.
  EXPECT_NE(source.find("CopyToClipboard(text)", copy), std::string::npos);
  EXPECT_LT(copy, source.find("\"ask_ai_message\"", for_request));
  const size_t text_check = source.rfind("if (slots.copy) {", copy); // the desktop's slot: a message with words
  ASSERT_NE(text_check, std::string::npos);
  EXPECT_EQ(source.find("\"reply_message\"", text_check), std::string::npos)
      << "Copy's own `if (slots.copy)` is the one after Forward / Select";
  ChatPresenter presenter;
  ForwardPorts ports;
  ports.active_thread = []() { return std::string("t"); };
  ports.get_message = [](const std::string&, const std::string&) {
    return std::optional<ThreadMessage>(TextMessage("hi"));
  };
  presenter.SetForwardPortsForTest(ports);
  EXPECT_FALSE(presenter.IsAttachmentMessage("m")) << "text is not an attachment: Copy stays";
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
