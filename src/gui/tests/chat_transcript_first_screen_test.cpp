// DEV/PLAN-chat-first-screen-rows.md: a chat switched to opens with its newest 30 shown messages (a group 50), not a
// 100-row page; older ones come a page at a time when scrolled up, or right away when the rows do not fill the screen.
// The real chat.rml on the real stylesheets (as chat_transcript_remount_test.cpp), its `chat-messages` given a set
// height so the visible height and the content height (which grows with the rows) are known; the store is a test one
// the scroller reads through StoreReads, its window drawn through the real AssembleTranscript as
// InboxController::BuildTranscript does (reactions folded onto their messages, edits never shown).
#include "common/chat/MessagingLimits.h"
#include "gui/UiFrameStats.h"
#include "gui/chat/ChatController.h"
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/ChatOpenMetrics.h"
#include "gui/chat/ChatScreenState.h"
#include "gui/chat/ChatTranscriptScroller.h"
#include "gui/shell/DataModelHost.h"
#include "gui/shell/ShellBaseHide.h"
#include "foundation/diagnostics/OsCurrentMemory.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/delivery/ReactionTypes.h"
#include "lib/messaging/TranscriptAssembly.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
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

/** The file with "\r\n" folded to "\n" (a Windows checkout). */
std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

std::string Asset(const char* relative) {
  return ReadFile(std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative);
}

/** The text of a function from its definition line to the closing brace at column 0. */
std::string Body(const std::string& file, const std::string& signature) {
  const std::string source = ReadFile(std::string(PP_SOURCE_DIR) + "/" + file);
  const size_t at = source.find("\n" + signature);
  EXPECT_NE(at, std::string::npos) << signature << " not found in " << file;
  if (at == std::string::npos) {
    return {};
  }
  const size_t close = source.find("\n}\n", at);
  return source.substr(at + 1, close == std::string::npos ? std::string::npos : close + 2 - (at + 1));
}

ThreadMessage Text(const std::string& thread, const int64_t order) {
  ThreadMessage m;
  m.id = "m" + std::to_string(order);
  m.thread_id = thread;
  m.sender_contact_id = "contact-1";
  m.content_type = ChatContentType::Text;
  m.text = "message " + std::to_string(order);
  m.display_order = order;
  return m;
}

/** An annotation row: a reaction folds onto its message; an edit never shows. */
ThreadMessage Annotation(const std::string& thread, const int64_t order, const std::string& target, const char* type) {
  ThreadMessage m;
  m.id = "a" + std::to_string(order);
  m.thread_id = thread;
  m.sender_contact_id = "contact-1";
  m.content_type = ChatContentType::Annotation;
  m.text = std::string(type) == kAnnotationTypeEdit ? "new words" : "+1";
  m.target_message_id = target;
  m.payload_json = BuildReactionPayloadJson(type, target, m.text);
  m.display_order = order;
  return m;
}

/** The chats' messages (oldest first) and every read the scroller makes of them. */
struct TestStore {
  std::map<std::string, std::vector<ThreadMessage>> threads;
  std::string active;
  bool fail = false;
  int page_reads = 0;
  std::vector<int> page_limits;     ///< the row count each page read asked for
  std::vector<std::optional<int64_t>> page_befores; ///< where each page read started (exclusive)
  size_t page_rows_read = 0;        ///< rows the page reads returned, all together
  int before_reads = 0;
  int row_builds = 0;
  std::vector<std::optional<int64_t>> row_bounds; ///< the oldest bound of each Rows call

  int Reads() const { return page_reads + before_reads + row_builds; }

  Roe<std::vector<ThreadMessage>> Page(const std::string& thread, const std::optional<int64_t> before, const int limit) {
    ++page_reads;
    page_limits.push_back(limit);
    page_befores.push_back(before);
    if (fail) {
      return Error("store not readable");
    }
    std::vector<ThreadMessage> older;
    for (const ThreadMessage& m : threads[thread]) {
      if (!before || m.display_order < *before) {
        older.push_back(m);
      }
    }
    if (older.size() > static_cast<size_t>(limit)) {
      older.erase(older.begin(), older.end() - limit);
    }
    page_rows_read += older.size();
    return older;
  }
  bool Before(const std::string& thread, const int64_t order) {
    ++before_reads;
    const auto& all = threads[thread];
    return std::any_of(all.begin(), all.end(), [&](const ThreadMessage& m) { return m.display_order < order; });
  }
  /** InboxController::BuildTranscript: one page from the newest without a bound, else down to the bound. */
  std::vector<MessageDisplayRow> Rows(const std::string& thread, const std::optional<int64_t> oldest,
                                      const std::optional<int64_t> newest) {
    ++row_builds;
    row_bounds.push_back(oldest);
    std::vector<ThreadMessage> window;
    const auto& all = threads[thread];
    for (const ThreadMessage& m : all) {
      if ((!oldest || m.display_order >= *oldest) && (!newest || m.display_order <= *newest)) {
        window.push_back(m);
      }
    }
    if (!oldest && window.size() > kDefaultMessagesPageSize) {
      window.erase(window.begin(), window.end() - static_cast<std::ptrdiff_t>(kDefaultMessagesPageSize));
    }
    std::vector<TranscriptRow> entries = AssembleTranscript(window, nullptr, ThreadKind::Direct);
    // Without a bound, a page showing nothing goes back for older pages until something shows (at most 20 more).
    for (int extra = 0; extra < 20 && entries.empty() && !oldest && !window.empty(); ++extra) {
      std::vector<ThreadMessage> older;
      for (const ThreadMessage& m : all) {
        if (m.display_order < window.front().display_order) {
          older.push_back(m);
        }
      }
      if (older.empty()) {
        break;
      }
      if (older.size() > kDefaultMessagesPageSize) {
        older.erase(older.begin(), older.end() - static_cast<std::ptrdiff_t>(kDefaultMessagesPageSize));
      }
      window.insert(window.begin(), older.begin(), older.end());
      entries = AssembleTranscript(window, nullptr, ThreadKind::Direct);
    }
    std::vector<MessageDisplayRow> rows;
    for (const TranscriptRow& entry : entries) {
      MessageDisplayRow row;
      row.message_id = entry.message.id.c_str();
      row.display_order = entry.message.display_order;
      row.row_class = entry.orphan_annotation ? "message-row-annotation-orphan" : "message-row-peer";
      row.content_rml = "<div class=\"bubble\"><p>a message that takes some room</p></div>";
      row.has_content = true;
      rows.push_back(std::move(row));
    }
    return rows;
  }
};

constexpr int kWidth = 390;
constexpr int kHeight = 844;

class ChatFirstScreenTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext("chat-first-screen-test", {kWidth, kHeight});
    ASSERT_NE(context, nullptr);
    ASSERT_TRUE(DataModelHost::Instance().Register(context, "chat", [this](ui::DataModelConstructor& ctor) {
      BindChatScreenState(ctor, state);
      ctor.BindEventCallback("messages_scroll", [this](ui::DataModelHandle, ui::Event&, const ui::VariantList&) {
        if (scroller) {
          scroller->OnMessagesScroll();
        }
      });
    }));
    const std::string chat_body = LocalizationService::Instance().LocalizeText(Asset("views/chat.rml"));
    const std::string rml = "<rml><head><style>" + Asset("themes/components.rcss") + Asset("themes/colors-light.rcss") +
                            "</style></head><body style=\"padding: 0;\">"
                            "<div style=\"position: absolute; top: 0; left: 0; width: " +
                            std::to_string(kWidth) + "dp; height: " + std::to_string(kHeight) +
                            "dp;\">"
                            "<div class=\"shell-layer shell-layer-base shell-layer-compact\" id=\"shell-base-layer\">"
                            "<div class=\"shell-chat-overlay\" id=\"shell-chat-overlay\">"
                            "<div class=\"shell-pane-body\" id=\"pane-body-chat\">" +
                            chat_body + "</div></div></div></div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();

    scroller = std::make_unique<ChatTranscriptScroller>(
        context,
        ChatTranscriptScroller::View{state.show_jump_to_latest, state.jump_to_latest_count, state.messages,
                                     state.has_turns},
        messaging_ready);
    ChatTranscriptScroller::StoreReads reads;
    reads.active_thread_id = [this]() { return store.active; };
    reads.messages_page = [this](const std::string& thread, std::optional<int64_t> before, int limit) {
      return store.Page(thread, before, limit);
    };
    reads.has_messages_before = [this](const std::string& thread, int64_t order) {
      return store.Before(thread, order);
    };
    reads.message = [this](const std::string& thread, const std::string& id) -> Roe<std::optional<ThreadMessage>> {
      for (const ThreadMessage& m : store.threads[thread]) {
        if (m.id == id) {
          return std::optional<ThreadMessage>(m);
        }
      }
      return std::optional<ThreadMessage>();
    };
    scroller->BindStoreReads(std::move(reads));
    scroller->SetRowBuilder([this](const std::string& thread, std::optional<int64_t> oldest,
                                   std::optional<int64_t> newest) { return store.Rows(thread, oldest, newest); });
    scroller->SetDirtyTurns([]() { DirtyChatTurns(); });
    scroller->SetOnOlderLoaded([this]() { ++older_loaded; });
  }
  void TearDown() override {
    scroller.reset();
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext("chat-first-screen-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }

  ui::Element* Messages() { return document->GetElementById("chat-messages"); }
  /** The transcript's visible height (its client height), set in dp. */
  void SetVisibleHeight(const float height) {
    Messages()->SetProperty("bottom", "auto");
    Messages()->SetProperty("height", std::to_string(height) + "dp");
  }
  float ClientHeight() { return Messages()->GetClientHeight(); }
  /** The visible height set so the client height is `client` (whatever padding the stylesheet gives the element). */
  void SetClientHeight(const float client) {
    SetVisibleHeight(client);
    context->Update();
    SetVisibleHeight(client + (client - ClientHeight()));
    context->Update();
    ASSERT_NEAR(ClientHeight(), client, 0.5f);
  }
  /** Drains the frames until the scroll to the bottom has settled. */
  void Settle() {
    for (int frame = 0; frame < 80 && !scroller->Settled(); ++frame) {
      Frames(1);
    }
  }
  bool NeverTrimmed() { return state.messages.size() <= kMaxMessagesDomWindow && !scroller->LoadedMaxDisplayOrder(); }
  float MaxTop() { return std::max(0.f, Messages()->GetScrollHeight() - Messages()->GetClientHeight()); }
  /** ChatController::SyncDisplayFromThread's row steps, in its order, for the chat `thread`. */
  void Sync(const std::string& thread, const size_t first_screen) {
    store.active = thread;
    const bool changed = scroller->BeginDisplaySync(thread, first_screen);
    const std::string prev_tail = state.messages.empty() ? std::string() : state.messages.back().message_id.c_str();
    const size_t prev_count = state.messages.size();
    state.messages = scroller->Rows(thread, scroller->LoadedMinDisplayOrder(), scroller->LoadedMaxDisplayOrder());
    state.has_turns = !state.messages.empty();
    scroller->EndDisplaySync(changed, prev_tail, prev_count);
    DirtyChatTurns();
  }
  /** Frames go by: a layout pass, then the scroll policy (ChatController::AfterLayout). */
  void Frames(const int n) {
    for (int frame = 0; frame < n; ++frame) {
      context->Update();
      scroller->ApplyPolicy();
    }
  }
  /** The person drags the transcript to `top`: its scroll event runs OnMessagesScroll (the bound messages_scroll). */
  void ScrollTo(const float top) { Messages()->SetScrollTop(top); }
  /** Where a row's top is drawn, from the transcript's top edge. */
  float RowTopOnScreen(const std::string& id) {
    ui::Element* row = document->QuerySelector(("[message-id=\"" + id + "\"]").c_str());
    EXPECT_NE(row, nullptr) << id;
    return row ? row->GetAbsoluteOffset(ui::BoxArea::Border).y - Messages()->GetAbsoluteOffset(ui::BoxArea::Border).y
               : 0.f;
  }
  void AddTexts(const std::string& thread, const int64_t first, const int64_t count) {
    for (int64_t i = 0; i < count; ++i) {
      store.threads[thread].push_back(Text(thread, first + i));
    }
  }
  std::string FrontId() const { return state.messages.empty() ? std::string() : state.messages.front().message_id.c_str(); }
  std::string BackId() const { return state.messages.empty() ? std::string() : state.messages.back().message_id.c_str(); }
  bool AtBottom() { return std::abs(Messages()->GetScrollTop() - MaxTop()) <= 1.f; }

  NullRender renderer;
  std::unique_ptr<ChatTranscriptScroller> scroller;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  ChatScreenState state;
  bool messaging_ready = true;
  TestStore store;
  int older_loaded = 0;
};

using FirstScreen = ChatTranscriptScroller::FirstScreen;
constexpr size_t kDirect = kChatFirstScreenMessagesDirect;
constexpr size_t kGroup = kChatFirstScreenMessagesGroup;

// ---------------------------------------------------------------------------------------------------------------------
// Step 1: the first screen.

TEST_F(ChatFirstScreenTest, ADirectChatOpensWithItsNewest30AtTheBottomAndMoreToLoad) {
  SetVisibleHeight(600.f); // client height 600
  AddTexts("dm", 1, 300);
  Sync("dm", kDirect);
  Frames(10);
  ASSERT_EQ(state.messages.size(), 30u);
  EXPECT_EQ(FrontId(), "m271");
  EXPECT_EQ(BackId(), "m300");
  EXPECT_GT(MaxTop(), 72.f) << "30 rows overfill 600";
  EXPECT_TRUE(AtBottom());
  EXPECT_TRUE(scroller->HasMoreLocalHistory());
  EXPECT_EQ(scroller->FirstScreenResult(), FirstScreen::Ok);
  EXPECT_EQ(ChatTranscriptScroller::FirstScreenWord(FirstScreen::Ok).view(), "ok");
}

TEST_F(ChatFirstScreenTest, AGroupOpensWithItsNewest50) {
  SetVisibleHeight(600.f); // client height 600
  AddTexts("group", 1, 300);
  Sync("group", kGroup);
  Frames(10);
  ASSERT_EQ(state.messages.size(), 50u);
  EXPECT_EQ(FrontId(), "m251");
  EXPECT_EQ(BackId(), "m300");
  EXPECT_TRUE(AtBottom());
  EXPECT_TRUE(scroller->HasMoreLocalHistory());
}

TEST_F(ChatFirstScreenTest, ScrollingToTheTopOfTheFirstScreenLoadsTheNext40InPlace) {
  SetVisibleHeight(600.f); // client height 600
  AddTexts("dm", 1, 300);
  Sync("dm", kDirect);
  Frames(10);
  ASSERT_EQ(state.messages.size(), 30u);
  ScrollTo(40.f); // within the 72 px of the top: the scroll event loads the page (the rows drawn next frame)
  ASSERT_EQ(older_loaded, 1);
  const float before = RowTopOnScreen("m271"); // still the old layout
  Frames(1); // the rows laid out, then the position fix
  context->Update();
  EXPECT_EQ(state.messages.size(), 70u) << "not stuck on the first 30: 40 more";
  EXPECT_EQ(FrontId(), "m231");
  EXPECT_EQ(store.page_limits.back(), static_cast<int>(kChatOlderPageMessages));
  EXPECT_NEAR(RowTopOnScreen("m271"), before, 1.f) << "the message looked at stays where it was";
  EXPECT_TRUE(scroller->HasMoreLocalHistory());
}

TEST_F(ChatFirstScreenTest, AChatOf12ShowsThemAllWithNoBoundAndNothingMore) {
  SetVisibleHeight(600.f); // client height 600
  AddTexts("dm", 1, 12);
  Sync("dm", kDirect);
  Frames(10);
  EXPECT_EQ(state.messages.size(), 12u);
  ASSERT_FALSE(store.row_bounds.empty());
  EXPECT_FALSE(store.row_bounds.front().has_value()) << "drawn without a bound (the old path)";
  EXPECT_FALSE(scroller->HasMoreLocalHistory());
  EXPECT_EQ(scroller->FirstScreenResult(), FirstScreen::Short);
  EXPECT_EQ(ChatTranscriptScroller::FirstScreenWord(FirstScreen::Short).view(), "short");
}

TEST_F(ChatFirstScreenTest, AChatOfExactly30ShowsThemAllAndScrollingUpReadsNothing) {
  SetVisibleHeight(600.f); // client height 600
  AddTexts("dm", 1, 30);
  Sync("dm", kDirect);
  Frames(10);
  EXPECT_EQ(state.messages.size(), 30u);
  EXPECT_EQ(FrontId(), "m1");
  EXPECT_FALSE(scroller->HasMoreLocalHistory());
  ASSERT_GT(MaxTop(), 72.f);
  const int reads = store.Reads();
  ScrollTo(0.f);
  Frames(5);
  EXPECT_EQ(store.Reads(), reads);
  EXPECT_EQ(state.messages.size(), 30u);
}

TEST_F(ChatFirstScreenTest, ReactionsAmongTheNewestRowsDoNotShortenTheFirstScreen) {
  SetVisibleHeight(600.f); // client height 600
  // 300 messages; 5 reactions to recent ones stored among the newest rows (after t290, t292 … t298).
  int64_t order = 1;
  for (int text = 1; text <= 300; ++text) {
    ThreadMessage m = Text("dm", order++);
    m.id = "t" + std::to_string(text);
    store.threads["dm"].push_back(m);
    if (text >= 290 && text < 300 && text % 2 == 0) {
      store.threads["dm"].push_back(Annotation("dm", order++, m.id, kAnnotationTypeReaction));
    }
  }
  ASSERT_EQ(store.threads["dm"].size(), 305u);
  Sync("dm", kDirect);
  Frames(10);
  EXPECT_EQ(state.messages.size(), 30u) << "30 messages shown, the reactions on them";
  EXPECT_EQ(BackId(), "t300");
  EXPECT_EQ(FrontId(), "t271");
}

TEST_F(ChatFirstScreenTest, ANewestPageOfMostlyReactionsReadsASecondPage) {
  SetVisibleHeight(600.f); // client height 600
  AddTexts("dm", 1, 200);
  // The newest 100 rows: 10 messages, each with 9 reactions after it.
  int64_t order = 201;
  for (int i = 0; i < 10; ++i) {
    store.threads["dm"].push_back(Text("dm", order++));
    const std::string target = store.threads["dm"].back().id;
    for (int r = 0; r < 9; ++r) {
      store.threads["dm"].push_back(Annotation("dm", order++, target, kAnnotationTypeReaction));
    }
  }
  Sync("dm", kDirect);
  EXPECT_EQ(store.page_reads, 2) << "a second page for the 20 messages still missing";
  Frames(10);
  EXPECT_EQ(state.messages.size(), 30u);
  EXPECT_EQ(FrontId(), "m181");
  EXPECT_EQ(scroller->FirstScreenResult(), FirstScreen::Ok);
}

TEST_F(ChatFirstScreenTest, ThreePagesOfHiddenRowsKeepTheOldWindow) {
  SetVisibleHeight(600.f); // client height 600
  // The newest 300 rows (101..400) are edits, which never show; messages 1..50 are older still.
  AddTexts("dm", 1, 50);
  for (int64_t order = 51; order <= 400; ++order) {
    store.threads["dm"].push_back(Annotation("dm", order, "m1", kAnnotationTypeEdit));
  }
  Sync("dm", kDirect);
  EXPECT_EQ(store.page_reads, 3) << "no fourth page";
  ASSERT_FALSE(store.row_bounds.empty());
  EXPECT_FALSE(store.row_bounds.back().has_value()) << "no bound: the old path";
  EXPECT_EQ(scroller->FirstScreenResult(), FirstScreen::Short);
  EXPECT_EQ(state.messages.size(), 50u) << "not blank: the old path goes back to the messages";
  EXPECT_EQ(BackId(), "m50");
  // Decided: the next sync does not read the first screen again.
  const int page_reads = store.page_reads;
  Frames(10);
  Sync("dm", kDirect);
  EXPECT_EQ(store.page_reads, page_reads);
}

TEST_F(ChatFirstScreenTest, ThreePagesWithOnly10MessagesKeepTheOldPage) {
  SetVisibleHeight(600.f); // client height 600
  // 1..200 messages; the newest 300 rows (201..500) hold 10 messages (every 30th) among edits.
  AddTexts("dm", 1, 200);
  for (int64_t order = 201; order <= 500; ++order) {
    store.threads["dm"].push_back(order % 30 == 0 ? Text("dm", order) : Annotation("dm", order, "m1", kAnnotationTypeEdit));
  }
  Sync("dm", kDirect);
  EXPECT_EQ(store.page_reads, 3);
  EXPECT_FALSE(store.row_bounds.back().has_value()) << "no bound";
  EXPECT_EQ(scroller->FirstScreenResult(), FirstScreen::Short);
  std::vector<std::string> shown;
  for (const MessageDisplayRow& row : state.messages) {
    shown.emplace_back(row.message_id.c_str());
  }
  std::vector<std::string> old_page;
  for (const MessageDisplayRow& row : store.Rows("dm", std::nullopt, std::nullopt)) {
    old_page.emplace_back(row.message_id.c_str());
  }
  EXPECT_EQ(shown, old_page) << "the window the change found";
  EXPECT_EQ(shown, (std::vector<std::string>{"m420", "m450", "m480"}));
  EXPECT_TRUE(scroller->HasMoreLocalHistory());
}

TEST_F(ChatFirstScreenTest, AFailedReadSetsNoBoundAndTheNextSyncDoes) {
  SetVisibleHeight(600.f); // client height 600
  AddTexts("dm", 1, 300);
  store.fail = true;
  Sync("dm", kDirect);
  store.fail = false; // only the first-screen read failed in this test; the rows after it read fine
  EXPECT_FALSE(store.row_bounds.back().has_value());
  EXPECT_EQ(state.messages.size(), 100u) << "the old first page";
  EXPECT_EQ(scroller->FirstScreenResult(), FirstScreen::Miss);
  EXPECT_EQ(ChatTranscriptScroller::FirstScreenWord(FirstScreen::Miss).view(), "miss");
  Frames(10);
  Sync("dm", kDirect); // the next sync (a new message, a redraw): the store answers now
  Frames(10);
  EXPECT_EQ(state.messages.size(), 30u);
  EXPECT_EQ(FrontId(), "m271");
  EXPECT_FALSE(scroller->LoadedMaxDisplayOrder().has_value());
  EXPECT_EQ(scroller->FirstScreenResult(), FirstScreen::Ok);
  EXPECT_TRUE(AtBottom());
}

TEST_F(ChatFirstScreenTest, NotReadyWhenSwitchedSetsNoBoundAndTheNextSyncDoes) {
  SetVisibleHeight(600.f); // client height 600
  AddTexts("dm", 1, 300);
  messaging_ready = false;
  Sync("dm", kDirect);
  EXPECT_EQ(scroller->FirstScreenResult(), FirstScreen::Miss);
  EXPECT_EQ(store.page_reads, 0);
  messaging_ready = true;
  Frames(10);
  Sync("dm", kDirect);
  EXPECT_EQ(state.messages.size(), 30u);
  EXPECT_EQ(scroller->FirstScreenResult(), FirstScreen::Ok);
}

TEST_F(ChatFirstScreenTest, AfterAFailedReadAWindowScrolledUpIsLeftAlone) {
  SetVisibleHeight(600.f); // client height 600
  AddTexts("dm", 1, 300);
  store.fail = true;
  Sync("dm", kDirect);
  store.fail = false;
  Frames(10);
  ASSERT_EQ(state.messages.size(), 100u);
  ScrollTo(MaxTop() / 2.f); // reading up the page
  Frames(2);
  const int page_reads = store.page_reads;
  Sync("dm", kDirect);
  Frames(2);
  EXPECT_EQ(state.messages.size(), 100u) << "the window is not cut under the reader";
  EXPECT_EQ(store.page_reads, page_reads) << "no first-screen read";
  EXPECT_EQ(scroller->FirstScreenResult(), FirstScreen::Miss);
}

TEST_F(ChatFirstScreenTest, ReopenJumpAndBackToLatestKeepTheirWindows) {
  SetVisibleHeight(600.f); // client height 600
  AddTexts("dm", 1, 300);
  Sync("dm", kDirect);
  Frames(10);
  ASSERT_EQ(state.messages.size(), 30u);
  // The same chat again (left and opened, or any later sync): the window stays.
  const int page_reads = store.page_reads;
  Sync("dm", kDirect);
  Frames(5);
  EXPECT_EQ(state.messages.size(), 30u);
  EXPECT_EQ(FrontId(), "m271");
  EXPECT_EQ(store.page_reads, page_reads);
  // A jump to a message older than the 250th: the window holds it.
  ASSERT_EQ(scroller->JumpToMessage("m40"), ChatTranscriptScroller::JumpResult::Jumped);
  EXPECT_TRUE(std::any_of(state.messages.begin(), state.messages.end(),
                          [](const MessageDisplayRow& row) { return row.message_id == "m40"; }));
  Frames(10);
  // Back to the latest: the window ends at the newest message again.
  scroller->OnJumpToLatest();
  Frames(10);
  EXPECT_EQ(BackId(), "m300");
  EXPECT_FALSE(scroller->LoadedMaxDisplayOrder().has_value());
  EXPECT_TRUE(AtBottom());
}

// ---------------------------------------------------------------------------------------------------------------------
// Step 2: a first screen that does not fill the transcript loads a page once the scroll to the bottom has settled.

TEST_F(ChatFirstScreenTest, NoFillWhileNotLaidOut) {
  SetVisibleHeight(0.f); // client height 0: as before the first layout
  Messages()->SetProperty("padding", "0px");
  AddTexts("dm", 1, 300);
  Sync("dm", kDirect);
  const int reads = store.Reads();
  Frames(40);
  EXPECT_EQ(ClientHeight(), 0.f);
  EXPECT_EQ(store.Reads(), reads);
  EXPECT_EQ(state.messages.size(), 30u);
}

TEST_F(ChatFirstScreenTest, NoFillWhileHidden) {
  SetClientHeight(4000.f); // client height 4000: 30 rows do not fill it
  AddTexts("dm", 1, 300);
  ShellBaseHide::Apply(context, document, true); // a full-screen call page over the chat
  Sync("dm", kDirect);
  const int reads = store.Reads();
  Frames(40);
  EXPECT_EQ(store.Reads(), reads);
  EXPECT_EQ(state.messages.size(), 30u);
  ShellBaseHide::Apply(context, document, false);
  Settle();
  Frames(2);
  EXPECT_EQ(state.messages.size(), 130u) << "shown again: filled";
}

TEST_F(ChatFirstScreenTest, NoFillWhileTheScrollToTheBottomSettles) {
  SetClientHeight(4000.f); // client height 4000
  AddTexts("dm", 1, 300);
  Sync("dm", kDirect);
  const int reads = store.Reads();
  int unsettled_frames = 0;
  while (!scroller->Settled() && unsettled_frames < 80) {
    Frames(1); // the frame that settles it included
    ++unsettled_frames;
    EXPECT_EQ(store.Reads(), reads) << "read during the settle, frame " << unsettled_frames;
  }
  EXPECT_GT(unsettled_frames, 1);
  EXPECT_EQ(state.messages.size(), 30u);
}

TEST_F(ChatFirstScreenTest, ATallScreenFillsOnceAndStaysAtTheBottom) {
  SetClientHeight(4000.f); // client height 4000
  AddTexts("dm", 1, 300);
  Sync("dm", kDirect);
  Settle();
  ASSERT_EQ(state.messages.size(), 30u);
  ASSERT_LE(MaxTop(), 72.f) << "30 rows do not fill 4000";
  Frames(1); // the check after the settle: one page
  EXPECT_EQ(state.messages.size(), 130u);
  EXPECT_EQ(older_loaded, 1);
  EXPECT_EQ(store.page_limits.back(), 100) << "a fill still loads 100";
  Settle();
  Frames(1);
  EXPECT_GT(MaxTop(), 72.f);
  EXPECT_TRUE(AtBottom()) << "back on the newest message";
  const int reads = store.Reads();
  Frames(30);
  EXPECT_EQ(store.Reads(), reads) << "over 100 rows: no more";
  EXPECT_EQ(state.messages.size(), 130u);
  EXPECT_TRUE(NeverTrimmed());
}

TEST_F(ChatFirstScreenTest, ContentJust50PxTallerThanTheScreenFillsToo) {
  SetClientHeight(400.f); // client height 400: 30 rows overfill it
  AddTexts("dm", 1, 300);
  Sync("dm", kDirect);
  Settle();
  Frames(3);
  ASSERT_EQ(state.messages.size(), 30u);
  const float content = Messages()->GetScrollHeight();
  SetClientHeight(content - 50.f); // client height = content - 50
  ASSERT_NEAR(MaxTop(), 50.f, 0.5f);
  Frames(1);
  EXPECT_EQ(state.messages.size(), 130u);
  Settle();
  Frames(1);
  EXPECT_TRUE(AtBottom());
  EXPECT_TRUE(NeverTrimmed());
}

TEST_F(ChatFirstScreenTest, Content500PxTallerThanTheScreenDoesNotFill) {
  SetClientHeight(400.f); // client height 400
  AddTexts("dm", 1, 300);
  Sync("dm", kDirect);
  Settle();
  Frames(3);
  const float content = Messages()->GetScrollHeight();
  SetClientHeight(content - 500.f); // client height = content - 500
  ASSERT_NEAR(MaxTop(), 500.f, 0.5f);
  const int reads = store.Reads();
  Frames(30);
  EXPECT_EQ(store.Reads(), reads);
  EXPECT_EQ(state.messages.size(), 30u);
}

TEST_F(ChatFirstScreenTest, AChatThatCannotFillLoadsOnceThenStops) {
  SetClientHeight(4000.f); // client height 4000
  AddTexts("dm", 1, 40);
  Sync("dm", kDirect);
  Settle();
  Frames(1);
  EXPECT_EQ(state.messages.size(), 40u);
  EXPECT_FALSE(scroller->HasMoreLocalHistory());
  Settle();
  const int reads = store.Reads();
  Frames(30);
  EXPECT_EQ(store.Reads(), reads) << "nothing older: no read each frame";
}

TEST_F(ChatFirstScreenTest, NoFillWhenScrolledUp) {
  SetClientHeight(400.f); // client height 400
  AddTexts("dm", 1, 300);
  Sync("dm", kDirect);
  Settle();
  Frames(3);
  const float content = Messages()->GetScrollHeight();
  SetClientHeight(content - 300.f); // client height = content - 300
  Frames(3);
  ScrollTo(150.f); // reading up: not near the bottom, not near the top
  Frames(2);
  ASSERT_EQ(older_loaded, 0);
  SetClientHeight(content - 70.f); // client height = content - 70: the window raised, 70 px left to scroll
  const int reads = store.Reads();
  Frames(30);
  ASSERT_LE(MaxTop(), 72.f);
  EXPECT_GT(MaxTop() - Messages()->GetScrollTop(), 64.f) << "still not at the bottom";
  EXPECT_EQ(store.Reads(), reads);
  EXPECT_EQ(state.messages.size(), 30u);
}

TEST_F(ChatFirstScreenTest, RaisedAgainItFillsAgainWhileAtMost100Rows) {
  // Newest 30 messages (171..200); the page before them holds 10 messages among 90 edits (71..170); 1..70 messages.
  AddTexts("dm", 1, 70);
  for (int64_t order = 71; order <= 170; ++order) {
    store.threads["dm"].push_back(order % 10 == 0 ? Text("dm", order) : Annotation("dm", order, "m1", kAnnotationTypeEdit));
  }
  AddTexts("dm", 171, 30);
  SetClientHeight(400.f); // client height 400: 30 rows overfill it
  Sync("dm", kDirect);
  Settle();
  Frames(3);
  ASSERT_EQ(state.messages.size(), 30u);
  SetClientHeight(Messages()->GetScrollHeight() + 20.f); // client height = 30 rows + 20: not filled
  Settle();
  Frames(1);
  ASSERT_EQ(state.messages.size(), 40u) << "the page added 10 messages";
  Settle();
  Frames(3);
  ASSERT_GT(MaxTop(), 72.f) << "40 rows fill it";
  ASSERT_EQ(older_loaded, 1);
  SetClientHeight(Messages()->GetScrollHeight() + 20.f); // client height = 40 rows + 20: not filled, 40 <= 100 rows
  Settle();
  Frames(1);
  EXPECT_EQ(older_loaded, 2);
  EXPECT_EQ(state.messages.size(), 110u);
  EXPECT_TRUE(NeverTrimmed());
}

TEST_F(ChatFirstScreenTest, RaisedAgainItDoesNotFillOver100Rows) {
  SetClientHeight(4000.f); // client height 4000
  AddTexts("dm", 1, 300);
  Sync("dm", kDirect);
  Settle();
  Frames(1);
  Settle();
  ASSERT_EQ(state.messages.size(), 130u);
  SetClientHeight(Messages()->GetScrollHeight() + 200.f); // client height = 130 rows + 200: not filled, 130 rows
  const int reads = store.Reads();
  Frames(30);
  EXPECT_EQ(store.Reads(), reads);
  EXPECT_EQ(state.messages.size(), 130u);
  EXPECT_TRUE(NeverTrimmed());
}

TEST_F(ChatFirstScreenTest, PagesWithNothingToShowStopAfterThree) {
  // Newest 30 messages (601..630); before them 5 pages of edits (101..600); then messages 1..100.
  AddTexts("dm", 1, 100);
  for (int64_t order = 101; order <= 600; ++order) {
    store.threads["dm"].push_back(Annotation("dm", order, "m1", kAnnotationTypeEdit));
  }
  AddTexts("dm", 601, 30);
  SetClientHeight(400.f); // client height 400
  Sync("dm", kDirect);
  Settle();
  Frames(3);
  ASSERT_EQ(state.messages.size(), 30u);
  SetClientHeight(Messages()->GetScrollHeight() - 50.f); // client height = 30 rows - 50: 50 px to scroll
  const int page_reads = store.page_reads;
  for (int frame = 0; frame < 60; ++frame) {
    Frames(1);
  }
  EXPECT_EQ(store.page_reads - page_reads, 3) << "three pages that added nothing, then it stops";
  EXPECT_EQ(older_loaded, 3);
  EXPECT_EQ(state.messages.size(), 30u);
  const int reads = store.Reads();
  Frames(30);
  EXPECT_EQ(store.Reads(), reads) << "no read each frame";
  // The person scrolls up: the scroll path still loads (three pages of 40 edits in a row, then it waits again).
  ScrollTo(0.f);
  EXPECT_EQ(older_loaded, 6);
  EXPECT_EQ(store.page_reads - page_reads, 6);
  EXPECT_TRUE(NeverTrimmed());
}

// ---------------------------------------------------------------------------------------------------------------------
// Step 3: chat.window says what a page cost. The scroller keeps the page's reading; ChatController writes it in the one
// chat.window line a second after the page (ChatWindowLine).

TEST_F(ChatFirstScreenTest, AScrolledPageLeavesOneReadingWithNoFrameBeforeTheFix) {
  SetClientHeight(600.f); // client height 600
  AddTexts("dm", 1, 300);
  Sync("dm", kDirect);
  Settle();
  Frames(2);
  ScrollTo(40.f);
  EXPECT_EQ(older_loaded, 1) << "one chat.window per page";
  Frames(1); // laid out, then the position fix, before any frame is drawn
  Frames(3);
  EXPECT_EQ(older_loaded, 1);
  const std::optional<ChatPageReading> page = scroller->TakePageReading();
  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(page->added, 40);
  EXPECT_FALSE(page->fill);
  EXPECT_EQ(page->frames_before_fix, std::optional<int64_t>(0));
  EXPECT_FALSE(scroller->TakePageReading().has_value()) << "taken once";
}

TEST_F(ChatFirstScreenTest, AFrameDrawnBeforeTheFixIsCounted) {
  SetClientHeight(600.f); // client height 600
  AddTexts("dm", 1, 300);
  Sync("dm", kDirect);
  Settle();
  Frames(2);
  ScrollTo(40.f);
  UiFrameStats::AddFrame(0, 0); // the main loop drew a frame before the scroll policy ran
  Frames(1);
  const std::optional<ChatPageReading> page = scroller->TakePageReading();
  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(page->frames_before_fix, std::optional<int64_t>(1));
}

TEST_F(ChatFirstScreenTest, AFillPageIsReadAsFillWithoutAFix) {
  SetClientHeight(4000.f); // client height 4000
  AddTexts("dm", 1, 300);
  Sync("dm", kDirect);
  Settle();
  Frames(1);
  ASSERT_EQ(state.messages.size(), 130u);
  EXPECT_EQ(older_loaded, 1) << "one chat.window per page";
  Settle();
  Frames(3);
  const std::optional<ChatPageReading> page = scroller->TakePageReading();
  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(page->added, 100);
  EXPECT_TRUE(page->fill);
  EXPECT_FALSE(page->frames_before_fix.has_value()) << "lands on the bottom: no position fix";
}

TEST_F(ChatFirstScreenTest, ASwitchDropsTheLastChatsReading) {
  SetClientHeight(600.f); // client height 600
  AddTexts("dm", 1, 300);
  AddTexts("other", 1, 300);
  Sync("dm", kDirect);
  Settle();
  Frames(2);
  ScrollTo(40.f);
  Frames(2);
  Sync("other", kDirect);
  EXPECT_FALSE(scroller->TakePageReading().has_value());
}

TEST(ChatWindowLineTest, APageLineKeepsTheOldFieldsAndAddsThePagesOnes) {
  std::vector<MessageDisplayRow> rows(3);
  rows[1].content_rml = "<div class=\"chat-attachment-clip\"><img class=\"chat-attachment-image\" src=\"/a.jpg\"/></div>";
  const CurrentMemoryMb memory{.footprint_mb = 210, .rss_mb = 180};
  EXPECT_EQ(ChatWindowLine(rows, /*exited=*/false, ChatPageReading{.added = 100, .fill = false, .frames_before_fix = 1},
                           memory)
                .str(),
            "event=chat.window rows=3 pictures=1 footprint_mb=210 rss_mb=180 added=100 cause=scroll "
            "frames_before_fix=1");
  EXPECT_EQ(ChatWindowLine(rows, false, ChatPageReading{.added = 100, .fill = true}, memory).str(),
            "event=chat.window rows=3 pictures=1 footprint_mb=210 rss_mb=180 added=100 cause=fill");
  // A peer's older history page (no local page reading): the old line.
  EXPECT_EQ(ChatWindowLine(rows, false, std::nullopt, memory).str(),
            "event=chat.window rows=3 pictures=1 footprint_mb=210 rss_mb=180");
  // Left the chat: no page fields.
  EXPECT_EQ(ChatWindowLine(rows, /*exited=*/true, std::nullopt, memory).str(),
            "event=chat.window rows=0 footprint_mb=210 rss_mb=180");
}

TEST(ChatWindowLineTest, TheControllerWritesTheLineWithThePageReadingOnlyAfterAPage) {
  const std::string schedule =
      Body("src/gui/chat/ChatController.cpp", "void ChatController::ScheduleChatWindowMetric(");
  EXPECT_NE(schedule.find("ChatWindowLine(chat_.messages, exited,"), std::string::npos);
  EXPECT_NE(schedule.find("exited ? std::nullopt : scroller_.TakePageReading()"), std::string::npos);
  EXPECT_EQ(schedule.find("MetricsLine line(\"chat.window\")"), std::string::npos) << "one place builds the line";
}

// ---------------------------------------------------------------------------------------------------------------------
// Review follow-ups.

TEST_F(ChatFirstScreenTest, AllTextTheBoundReadsOnlyTheFirstScreensRows) {
  SetVisibleHeight(600.f); // client height 600
  AddTexts("dm", 1, 300);
  Sync("dm", kDirect);
  EXPECT_EQ(store.page_limits, (std::vector<int>{30})) << "one read of 30 rows, not a page of 100";
  EXPECT_EQ(store.page_rows_read, 30u);
  EXPECT_EQ(state.messages.size(), 30u);
  EXPECT_EQ(FrontId(), "m271");
}

TEST_F(ChatFirstScreenTest, AGroupBoundReadsFiftyRows) {
  SetVisibleHeight(600.f); // client height 600
  AddTexts("group", 1, 300);
  Sync("group", kGroup);
  EXPECT_EQ(store.page_limits, (std::vector<int>{50}));
  EXPECT_EQ(store.page_rows_read, 50u);
}

TEST_F(ChatFirstScreenTest, AnnotationsAmongTheFirstRowsReadOnAPageOf100) {
  SetVisibleHeight(600.f); // client height 600
  AddTexts("dm", 1, 270);
  // The newest 30 rows: 15 messages each with a reaction after it.
  for (int64_t order = 271; order <= 300; order += 2) {
    store.threads["dm"].push_back(Text("dm", order));
    store.threads["dm"].push_back(Annotation("dm", order + 1, "m" + std::to_string(order), kAnnotationTypeReaction));
  }
  Sync("dm", kDirect);
  EXPECT_EQ(store.page_limits, (std::vector<int>{30, 100}));
  EXPECT_EQ(store.page_rows_read, 130u);
  EXPECT_EQ(state.messages.size(), 30u);
  EXPECT_EQ(FrontId(), "m256") << "15 there, 15 more from the page before";
  EXPECT_EQ(scroller->FirstScreenResult(), FirstScreen::Ok);
}

TEST_F(ChatFirstScreenTest, AfterAFailedReadARetryDoesNotCutTheWindowOnceMovedOffTheBottom) {
  SetClientHeight(600.f); // client height 600
  AddTexts("dm", 1, 300);
  store.fail = true;
  Sync("dm", kDirect);
  store.fail = false;
  Settle();
  Frames(2);
  ASSERT_EQ(state.messages.size(), 100u);
  ASSERT_TRUE(AtBottom());
  ScrollTo(MaxTop() - 30.f); // 30 px up: still within the 64 px that counts as pinned
  Frames(2);
  const int page_reads = store.page_reads;
  Sync("dm", kDirect); // the store answers now
  Frames(2);
  EXPECT_EQ(state.messages.size(), 100u) << "the window is not cut under the reader";
  EXPECT_EQ(scroller->FirstScreenResult(), FirstScreen::Miss);
  EXPECT_EQ(store.page_reads, page_reads);
  // Given up: later syncs do not try again either.
  ScrollTo(MaxTop());
  Frames(2);
  Sync("dm", kDirect);
  EXPECT_EQ(state.messages.size(), 100u);
  EXPECT_EQ(store.page_reads, page_reads);
}

TEST_F(ChatFirstScreenTest, AfterFiveFailedRetriesItStopsReading) {
  SetClientHeight(600.f); // client height 600
  AddTexts("dm", 1, 300);
  store.fail = true; // the first-screen reads keep failing (the rows are drawn by the row builder)
  Sync("dm", kDirect);
  EXPECT_EQ(store.page_reads, 1);
  for (int retry = 1; retry <= 5; ++retry) {
    Sync("dm", kDirect); // an AI answer streaming in: a sync per token
    EXPECT_EQ(store.page_reads, 1 + retry);
  }
  for (int more = 0; more < 10; ++more) {
    Sync("dm", kDirect);
  }
  EXPECT_EQ(store.page_reads, 6) << "five retries, then no more";
  EXPECT_EQ(scroller->FirstScreenResult(), FirstScreen::Miss);
}

TEST_F(ChatFirstScreenTest, AFillWithNothingOlderInTheStoreDoesNotLandAgain) {
  SetClientHeight(4000.f); // client height 4000: 30 rows do not fill it
  AddTexts("dm", 1, 40);
  ASSERT_TRUE(store.threads["dm"].size() == 40u);
  messaging_ready = true;
  Sync("dm", kDirect);
  ASSERT_TRUE(scroller->HasMoreLocalHistory());
  // The older ten went meanwhile (deleted on another device): the store has nothing before the window now.
  store.threads["dm"].erase(store.threads["dm"].begin(), store.threads["dm"].begin() + 10);
  Settle();
  Frames(1); // the fill check: no page
  EXPECT_FALSE(scroller->HasMoreLocalHistory());
  EXPECT_TRUE(scroller->Settled()) << "no new scroll to the bottom";
  EXPECT_EQ(older_loaded, 0);
  EXPECT_FALSE(scroller->TakePageReading().has_value());
  const int reads = store.Reads();
  Frames(30);
  EXPECT_EQ(store.Reads(), reads) << "not tried again";
}

TEST_F(ChatFirstScreenTest, AJumpInsideTheOldPageButOutsideTheFirstScreenRedrawsTheWindow) {
  SetClientHeight(600.f); // client height 600
  AddTexts("dm", 1, 300);
  Sync("dm", kDirect);
  Settle();
  Frames(2);
  ASSERT_EQ(FrontId(), "m271");
  // m230: the 71st newest, inside the old 100-row page, outside the 30-row first screen.
  ASSERT_EQ(scroller->JumpToMessage("m230"), ChatTranscriptScroller::JumpResult::Jumped);
  EXPECT_EQ(FrontId(), "m230") << "the window drawn again from it";
  Frames(10);
  context->Update();
  const float top = RowTopOnScreen("m230");
  EXPECT_GE(top, -1.f);
  EXPECT_LT(top, ClientHeight()) << "in view";
  scroller->OnJumpToLatest();
  Settle();
  Frames(2);
  EXPECT_EQ(BackId(), "m300");
  EXPECT_FALSE(scroller->LoadedMaxDisplayOrder().has_value());
  EXPECT_TRUE(AtBottom());
}

// ---------------------------------------------------------------------------------------------------------------------
// Scrolling up loads kChatOlderPageMessages (40) stored rows at a time (device data 2026-11-12: a 95-row page cost a
// 203 ms frame).

TEST_F(ChatFirstScreenTest, FourScrolledPagesThenTheFifthTrimsAsBefore) {
  SetClientHeight(600.f); // client height 600
  AddTexts("dm", 1, 300);
  Sync("dm", kDirect);
  Settle();
  Frames(2);
  for (int page = 1; page <= 4; ++page) {
    ScrollTo(40.f);
    ASSERT_EQ(older_loaded, page);
    Frames(2);
    EXPECT_EQ(state.messages.size(), 30u + 40u * page);
  }
  EXPECT_EQ(FrontId(), "m111");
  EXPECT_EQ(scroller->LoadedMinDisplayOrder(), std::optional<int64_t>(111));
  EXPECT_FALSE(scroller->LoadedMaxDisplayOrder().has_value());
  EXPECT_TRUE(scroller->HasMoreLocalHistory());
  // The fifth goes over 200: the newest rows are dropped, as with any page.
  ScrollTo(40.f);
  Frames(2);
  EXPECT_EQ(state.messages.size(), kMaxMessagesDomWindow);
  EXPECT_EQ(FrontId(), "m71");
  EXPECT_EQ(BackId(), "m270");
  EXPECT_EQ(scroller->LoadedMaxDisplayOrder(), std::optional<int64_t>(270));
  EXPECT_TRUE(state.show_jump_to_latest);
}

TEST_F(ChatFirstScreenTest, AScrolledPageOfOnlyHiddenRowsReadsTheNextAtOnce) {
  SetClientHeight(600.f); // client height 600
  // 1..190 messages, 191..230 edits (a page of 40 that shows nothing), 231..300 messages.
  AddTexts("dm", 1, 190);
  for (int64_t order = 191; order <= 230; ++order) {
    store.threads["dm"].push_back(Annotation("dm", order, "m1", kAnnotationTypeEdit));
  }
  AddTexts("dm", 231, 70);
  Sync("dm", kDirect);
  Settle();
  Frames(2);
  ScrollTo(40.f); // 231..270
  Frames(2);
  ASSERT_EQ(state.messages.size(), 70u);
  const int page_reads = store.page_reads;
  ScrollTo(40.f); // 191..230 shows nothing: 151..190 read at once, no second gesture needed
  const float top = Messages()->GetScrollTop();
  Frames(2);
  EXPECT_EQ(store.page_reads - page_reads, 2);
  EXPECT_EQ(state.messages.size(), 110u);
  EXPECT_EQ(FrontId(), "m151");
  EXPECT_GT(Messages()->GetScrollTop(), top + 100.f) << "the old rows moved down by the new ones' height";
}

TEST_F(ChatFirstScreenTest, ThreeScrolledPagesOfHiddenRowsWaitForTheNextGesture) {
  SetClientHeight(600.f); // client height 600
  // 1..100 messages, 101..230 edits (more than three pages of 40), 231..300 messages.
  AddTexts("dm", 1, 100);
  for (int64_t order = 101; order <= 230; ++order) {
    store.threads["dm"].push_back(Annotation("dm", order, "m1", kAnnotationTypeEdit));
  }
  AddTexts("dm", 231, 70);
  Sync("dm", kDirect);
  Settle();
  Frames(2);
  ScrollTo(40.f); // 231..270
  Frames(2);
  ASSERT_EQ(state.messages.size(), 70u);
  int page_reads = store.page_reads;
  ScrollTo(30.f); // 191..230, 151..190, 111..150: nothing shown, it stops at three
  Frames(2);
  EXPECT_EQ(store.page_reads - page_reads, 3);
  EXPECT_EQ(state.messages.size(), 70u);
  EXPECT_TRUE(scroller->HasMoreLocalHistory());
  page_reads = store.page_reads;
  ScrollTo(20.f); // the next gesture near the top: 101..110 edits and 71..100 messages
  Frames(2);
  EXPECT_EQ(store.page_reads - page_reads, 1);
  EXPECT_EQ(state.messages.size(), 100u);
  EXPECT_EQ(FrontId(), "m71");
}

TEST_F(ChatFirstScreenTest, AFullWindowLoadsOnePagePerGesture) {
  SetClientHeight(600.f); // client height 600
  AddTexts("dm", 1, 400);
  Sync("dm", kDirect);
  Settle();
  Frames(2);
  for (int page = 1; page <= 5; ++page) {
    ScrollTo(40.f);
    Frames(2);
  }
  ASSERT_EQ(state.messages.size(), kMaxMessagesDomWindow);
  ASSERT_EQ(FrontId(), "m171");
  // The sixth and seventh gestures: each page is trimmed back to 200 rows, the window moves 40 older, one read each.
  for (const char* front : {"m131", "m91"}) {
    const std::string looked_at = FrontId();
    const int page_reads = store.page_reads;
    ScrollTo(40.f);
    const float before = RowTopOnScreen(looked_at); // the old layout
    Frames(1);
    context->Update();
    EXPECT_EQ(store.page_reads - page_reads, 1) << "one page for a gesture";
    EXPECT_EQ(state.messages.size(), kMaxMessagesDomWindow);
    EXPECT_EQ(FrontId(), front);
    EXPECT_NEAR(RowTopOnScreen(looked_at), before, 1.f) << "the message looked at stays where it was";
    Frames(1);
  }
}

TEST_F(ChatFirstScreenTest, ASyncKeepsTheBoundPastHiddenRows) {
  SetClientHeight(600.f); // client height 600
  // 1..100 messages, 400 edits (101..500), 501..600 messages.
  AddTexts("dm", 1, 100);
  for (int64_t order = 101; order <= 500; ++order) {
    store.threads["dm"].push_back(Annotation("dm", order, "m1", kAnnotationTypeEdit));
  }
  AddTexts("dm", 501, 100);
  Sync("dm", kDirect);
  Settle();
  Frames(2);
  ScrollTo(40.f); // 531..570
  Frames(2);
  ScrollTo(40.f); // 491..530: 501..530 shown
  Frames(2);
  ASSERT_EQ(FrontId(), "m501");
  ScrollTo(30.f); // 451..490, 411..450, 371..410: nothing shown, it stops at three
  Frames(2);
  ASSERT_EQ(scroller->LoadedMinDisplayOrder(), std::optional<int64_t>(371));
  std::vector<std::string> before_sync;
  for (const MessageDisplayRow& row : state.messages) {
    before_sync.emplace_back(row.message_id.c_str());
  }
  // A new message arrives (or an AI answer streams in): the display sync.
  store.threads["dm"].push_back(Text("dm", 601));
  Sync("dm", kDirect);
  Frames(2);
  std::vector<std::string> after_sync;
  for (const MessageDisplayRow& row : state.messages) {
    after_sync.emplace_back(row.message_id.c_str());
  }
  before_sync.push_back("m601");
  EXPECT_EQ(after_sync, before_sync) << "the same rows, and the new one";
  EXPECT_EQ(scroller->LoadedMinDisplayOrder(), std::optional<int64_t>(371)) << "the bound stays past the edits";
  EXPECT_TRUE(scroller->HasMoreLocalHistory());
  // The next gesture goes on from there; with a sync before each, it still gets through to the older messages.
  const size_t reads_before = store.page_befores.size();
  ScrollTo(20.f);
  Frames(2);
  ASSERT_GT(store.page_befores.size(), reads_before);
  EXPECT_EQ(store.page_befores[reads_before], std::optional<int64_t>(371)) << "not read again from 501";
  for (int gesture = 0; gesture < 4 && FrontId() != "m91"; ++gesture) {
    store.threads["dm"].push_back(Text("dm", 602 + gesture));
    Sync("dm", kDirect);
    Frames(2);
    ScrollTo(gesture % 2 == 0 ? 35.f : 25.f);
    Frames(2);
  }
  EXPECT_EQ(FrontId(), "m91");
}

TEST(ChatFirstScreenCountTest, FirstScreenMessagesByKind) {
  EXPECT_EQ(ChatController::FirstScreenMessages(ThreadKind::Group), 50u);
  EXPECT_EQ(ChatController::FirstScreenMessages(ThreadKind::Direct), 30u);
  EXPECT_EQ(ChatController::FirstScreenMessages(ThreadKind::Ai), 30u);
}

TEST(ChatFirstScreenCountTest, TheControllerPassesTheCountAndReportsTheResult) {
  // ChatController cannot be built in a unit test: that the switch passes the count is read from the source.
  const std::string sync = Body("src/gui/chat/ChatController.cpp", "void ChatController::SyncDisplayFromThread(");
  EXPECT_NE(sync.find("BeginDisplaySync("), std::string::npos);
  EXPECT_NE(sync.find("FirstScreenMessages("), std::string::npos);
  // chat.open says how the open's first screen went: recorded with the open's rows, written with its layout.
  EXPECT_NE(sync.find("FirstScreenResult()"), std::string::npos);
  const std::string open = Body("src/gui/chat/ChatController.cpp", "void ChatController::OnSyncLayoutMeasured(");
  EXPECT_NE(open.find("\"first_screen\""), std::string::npos);
  EXPECT_NE(open.find("FirstScreenWord("), std::string::npos);
}

} // namespace
} // namespace pbr
