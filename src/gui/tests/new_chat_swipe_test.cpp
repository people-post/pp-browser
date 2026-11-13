// DEV/PLAN-fewer-taps-1.md 4: a left swipe on a contact row of the phone New chat page shows Info / Voice / Video / More.
// The swipe gesture is configurable (the Chats list keeps its defaults); the contact rows carry `contact-id`.
#include "gui/contacts/ContactsController.h"
#include "gui/shell/SessionSwipeGesture.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/people/ContactTypes.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/EventListener.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

using Kind = SessionSwipeOutcome::Kind;

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// --- the model with the contact-row numbers ------------------------------------------------------------------------

SessionSwipeModel ContactModel() {
  return SessionSwipeModel(ContactRowSwipeConfig().swipe);
}

TEST(NewChatSwipeModelTest, FourSixtyDpButtonsOnTheRightAndNoneOnTheLeft) {
  SessionSwipeModel model = ContactModel();
  EXPECT_FLOAT_EQ(model.OpenWidthDp(SwipeSide::End), 240.f);
  EXPECT_FLOAT_EQ(model.OpenWidthDp(SwipeSide::Start), 0.f);
  EXPECT_FLOAT_EQ(model.RestOffsetDp(SwipeSide::End), -240.f);
}

TEST(NewChatSwipeModelTest, ALeftDragPastHalfOpensAndIsClampedAt240) {
  SessionSwipeModel model = ContactModel();
  model.Press("c1", 300, 100, 358.f);
  model.Update(290, 100, 1.f);
  EXPECT_FLOAT_EQ(model.Update(200, 100, 1.f).offset_dp, -100.f);
  EXPECT_FLOAT_EQ(model.Update(-500, 100, 1.f).offset_dp, -240.f);
  const SessionSwipeOutcome out = model.Release();
  EXPECT_EQ(out.kind, Kind::Open);
  EXPECT_EQ(out.side, SwipeSide::End);
  EXPECT_EQ(model.OpenRow(), "c1");

  SessionSwipeModel short_drag = ContactModel();
  short_drag.Press("c1", 300, 100, 358.f);
  short_drag.Update(290, 100, 1.f);
  short_drag.Update(200, 100, 1.f); // 100dp < half of 240
  EXPECT_EQ(short_drag.Release().kind, Kind::Close);
}

TEST(NewChatSwipeModelTest, ARightwardDragOnAClosedRowIsNotClaimed) {
  SessionSwipeModel model = ContactModel();
  model.Press("c1", 100, 100, 358.f);
  EXPECT_FALSE(model.Update(120, 100, 1.f).claimed) << "left free for the page's swipe back";
  EXPECT_FALSE(model.Update(220, 100, 1.f).claimed);
  const SessionSwipeOutcome out = model.Release();
  EXPECT_EQ(out.kind, Kind::None);
  EXPECT_FALSE(out.swallow_click);
}

TEST(NewChatSwipeModelTest, AnOpenRowCanBeDraggedBackRight) {
  SessionSwipeModel model = ContactModel();
  model.Press("c1", 300, 100, 358.f);
  model.Update(290, 100, 1.f);
  model.Update(0, 100, 1.f);
  ASSERT_EQ(model.Release().kind, Kind::Open);
  model.Press("c1", 100, 100, 358.f);
  EXPECT_TRUE(model.Update(120, 100, 1.f).claimed) << "closing an open row is ours";
}

TEST(NewChatSwipeModelTest, TheDefaultsAreTheChatsOnesAndARightDragStillClaims) {
  const SessionSwipeGestureConfig chats;
  EXPECT_EQ(chats.list_ids, (std::vector<std::string>{"sidebar-sessions", "home-ai-sessions", "ai-drawer-sessions"}));
  EXPECT_EQ(chats.row_class, "sidebar-session");
  EXPECT_EQ(chats.id_attribute, "session-id");
  EXPECT_FLOAT_EQ(chats.swipe.button_dp, 74.f);
  EXPECT_EQ(chats.swipe.end_buttons, 3);
  EXPECT_EQ(chats.swipe.start_buttons, 2);
  SessionSwipeModel model(chats.swipe);
  model.Press("t", 100, 100, 400.f);
  EXPECT_TRUE(model.Update(130, 100, 1.f).claimed);
}

// --- the page ------------------------------------------------------------------------------------------------------

TEST(NewChatSwipeMarkupTest, RowsCarryContactIdNotSessionIdAndHaveFourButtonsInOrder) {
  const std::string rml = ReadAsset("views/new_chat.rml");
  EXPECT_NE(rml.find("data-attr-contact-id=\"contact.id\""), std::string::npos);
  EXPECT_EQ(rml.find("data-attr-session-id"), std::string::npos) << "never taken for a chat";
  EXPECT_EQ(rml.find(" session-id="), std::string::npos);
  EXPECT_NE(rml.find("class=\"contacts-row\""), std::string::npos);
  EXPECT_NE(rml.find("id=\"new-chat-list\""), std::string::npos);
  size_t at = 0;
  for (const char* action :
       {"swipe-action=\"info\"", "swipe-action=\"voice\"", "swipe-action=\"video\"", "swipe-action=\"more\""}) {
    const size_t found = rml.find(action, at);
    ASSERT_NE(found, std::string::npos) << action;
    at = found;
  }
  EXPECT_EQ(rml.find("swipe-action=\"", at + 10), std::string::npos) << "exactly four";
  EXPECT_NE(rml.find("new_chat_pick(contact.id)"), std::string::npos) << "a tap still opens the chat";
  for (const char* key :
       {"new_chat.swipe.info", "new_chat.swipe.voice", "new_chat.swipe.video", "sidebar.swipe.more"}) {
    EXPECT_NE(rml.find(key), std::string::npos) << key;
  }
}

TEST(NewChatSwipeMarkupTest, TheButtonsAre60dpAndTheColoursComeFromTheTemplate) {
  const std::string css = ReadAsset("themes/components.rcss");
  const size_t button = css.find(".shell-layer-compact .nc-page .nc-swipe-btn {");
  ASSERT_NE(button, std::string::npos);
  EXPECT_NE(css.find("width: 60dp;", button), std::string::npos);
  EXPECT_LT(css.find("width: 60dp;", button), css.find('}', button));
  for (const char* name : {"info", "voice", "video", "more"}) {
    const std::string rule = std::string(".nc-swipe-btn--") + name + " {";
    EXPECT_NE(ReadAsset("themes/colors-light.rcss").find(rule), std::string::npos) << name;
    EXPECT_NE(ReadAsset("themes/colors-dark.rcss").find(rule), std::string::npos) << name;
  }
}

TEST(NewChatSwipeMarkupTest, TheNewTextsExistInBothLanguages) {
  for (const char* file : {"locales/en.json", "locales/zh-Hans.json"}) {
    const std::string json = ReadAsset(file);
    for (const char* key : {"\"new_chat.swipe.info\"", "\"new_chat.swipe.voice\"", "\"new_chat.swipe.video\"",
                            "\"contacts.call_blocked\""}) {
      EXPECT_NE(json.find(key), std::string::npos) << file << " " << key;
    }
  }
}

// --- the gesture, two instances ------------------------------------------------------------------------------------

class NullRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i&, const ui::String&) override { return 0; }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 0; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

class ClickCounter : public ui::EventListener {
public:
  void ProcessEvent(ui::Event&) override { ++clicks; }
  int clicks = 0;
};

// The Chats list on top (600px, as the Chats hit test) and the New chat list under it: contact rows 56px high in a
// 400px list that hides what overflows sideways, four 60px buttons just right of the row.
constexpr const char* kPageRml = R"(<rml><head><style>
body { display: block; width: 400px; height: 900px; margin: 0; }
div { display: block; }
#sidebar-sessions { width: 400px; height: 300px; overflow-x: hidden; }
.sidebar-session { position: relative; height: 60px; }
.session-swipe { display: none; }
.sidebar-session.session-swipe-shown .session-swipe { display: flex; flex-direction: row; position: absolute; top: 0; bottom: 0; }
.session-swipe--end { left: 100%; }
.session-swipe--start { right: 100%; }
.session-swipe-btn { width: 74px; height: 60px; }
#new-chat-list { width: 400px; height: 400px; overflow-x: hidden; }
.contacts-row { position: relative; height: 56px; }
.nc-swipe { display: none; }
.contacts-row.session-swipe-shown .nc-swipe { display: flex; flex-direction: row; position: absolute; top: 0; bottom: 0; left: 100%; }
.nc-swipe-btn { width: 60px; height: 56px; }
</style></head><body><div id="page">
<div id="sidebar-sessions">
<div class="sidebar-session" id="chat_row" session-id="t1">
  <div class="session-swipe session-swipe--end">
    <div class="session-swipe-btn" swipe-action="more"></div>
    <div class="session-swipe-btn" swipe-action="archive"></div>
    <div class="session-swipe-btn" swipe-action="delete"></div>
  </div>
  <div class="session-swipe session-swipe--start">
    <div class="session-swipe-btn" swipe-action="pin"></div>
    <div class="session-swipe-btn" swipe-action="read"></div>
  </div>
</div>
</div>
<div id="new-chat-list" style="margin-top: 20px;">
<div class="contacts-row" id="contact_row" contact-id="c1">
  <div class="nc-swipe">
    <div class="nc-swipe-btn" swipe-action="info"></div>
    <div class="nc-swipe-btn" swipe-action="voice"></div>
    <div class="nc-swipe-btn" swipe-action="video"></div>
    <div class="nc-swipe-btn" swipe-action="more"></div>
  </div>
</div>
<div class="contacts-row" id="fake_row" session-id="not-a-contact"></div>
</div>
</div></body></rml>)";

class NewChatSwipeHitTest : public testing::Test {
protected:
  static constexpr int kChatY = 30;
  static constexpr int kContactY = 350;

  void SetUp() override {
    ui::SetRenderInterface(&renderer_);
    initialized_ = ui::Initialise();
    ASSERT_TRUE(initialized_);
    context_ = ui::CreateContext("new-chat-swipe-hit-test", {400, 900});
    ASSERT_NE(context_, nullptr);
    document_ = context_->LoadDocumentFromMemory(kPageRml);
    ASSERT_NE(document_, nullptr);
    document_->Show();
    context_->Update();
    chat_row_ = document_->GetElementById("chat_row");
    contact_row_ = document_->GetElementById("contact_row");
    ASSERT_NE(chat_row_, nullptr);
    ASSERT_NE(contact_row_, nullptr);
    contact_row_->AddEventListener(ui::EventId::Click, &row_clicks_);
    chats_.Attach(document_->GetElementById("page"), context_,
                  [this](const std::string& id, const SessionSwipeAction action) {
                    chat_actions_.push_back(id + ":" + std::to_string(static_cast<int>(action)));
                  });
  }

  void TearDown() override {
    chats_.Detach();
    contacts_.Detach();
    if (contact_row_) {
      contact_row_->RemoveEventListener(ui::EventId::Click, &row_clicks_);
    }
    if (context_) {
      ui::RemoveContext("new-chat-swipe-hit-test");
    }
    if (initialized_) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }

  void AttachContacts() {
    contacts_.AttachNamed(
        document_->GetElementById("new-chat-list"), context_,
        [this](const std::string& id, const std::string& action) { contact_actions_.push_back(id + ":" + action); });
  }

  void Drag(const int from_x, const int to_x, const int y) {
    context_->ProcessMouseMove(from_x, y, 0);
    context_->ProcessMouseButtonDown(0, 0);
    for (int i = 1; i <= 10; ++i) {
      context_->ProcessMouseMove(from_x + (to_x - from_x) * i / 10, y, 0);
      context_->Update();
    }
    context_->ProcessMouseButtonUp(0, 0);
    context_->Update();
  }

  void Tap(const int x, const int y) {
    context_->ProcessMouseMove(x, y, 0);
    context_->ProcessMouseButtonDown(0, 0);
    context_->ProcessMouseButtonUp(0, 0);
    context_->Update();
  }

  // The result of the same two gestures on the Chats list: what the first instance does.
  std::vector<std::string> ChatsBehaviour() {
    chat_actions_.clear();
    Drag(380, 158, kChatY);
    const bool shown = chat_row_->IsClassSet("session-swipe-shown");
    Tap(363, kChatY); // Delete
    std::vector<std::string> out = chat_actions_;
    out.push_back(shown ? "shown" : "not shown");
    Drag(20, 168, kChatY); // right: Pin, Read
    out.push_back(chat_row_->IsClassSet("session-swipe-shown") ? "start shown" : "start not shown");
    Tap(37, kChatY);
    out.insert(out.end(), chat_actions_.begin() + 1, chat_actions_.end());
    return out;
  }

  NullRender renderer_;
  bool initialized_ = false;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ui::Element* chat_row_ = nullptr;
  ui::Element* contact_row_ = nullptr;
  ClickCounter row_clicks_;
  SessionSwipeGesture chats_;
  SessionSwipeGesture contacts_{ContactRowSwipeConfig()};
  std::vector<std::string> chat_actions_;
  std::vector<std::string> contact_actions_;
};

TEST_F(NewChatSwipeHitTest, EachOfTheFourButtonsGetsItsTapWithTheContactsId) {
  AttachContacts();
  const struct {
    int x;
    const char* action;
  } taps[] = {{190, "info"}, {250, "voice"}, {310, "video"}, {370, "more"}};
  for (const auto& tap : taps) {
    contact_actions_.clear();
    row_clicks_.clicks = 0;
    Drag(380, 140, kContactY); // 240px left: the four buttons exactly
    ASSERT_TRUE(contact_row_->IsClassSet("session-swipe-shown"));
    Tap(tap.x, kContactY);
    EXPECT_EQ(contact_actions_, std::vector<std::string>{std::string("c1:") + tap.action}) << tap.x;
    EXPECT_EQ(row_clicks_.clicks, 0) << "a button tap must not open the chat, x=" << tap.x;
  }
}

TEST_F(NewChatSwipeHitTest, ATapOnTheRowStillReachesItAndARightDragOnAClosedRowDoesNothing) {
  AttachContacts();
  Tap(100, kContactY);
  EXPECT_EQ(row_clicks_.clicks, 1) << "a plain tap opens the chat as before";
  Drag(20, 200, kContactY);
  EXPECT_FALSE(contact_row_->IsClassSet("session-swipe-shown"));
  EXPECT_TRUE(contact_actions_.empty());
}

TEST_F(NewChatSwipeHitTest, AContactRowIsNotAChatAndAChatRowIsNotAContact) {
  AttachContacts();
  ui::Element* fake = document_->GetElementById("fake_row");
  ASSERT_NE(fake, nullptr);
  Drag(380, 140, kContactY + 56); // the row with a session-id and no contact-id: not a contact row
  EXPECT_FALSE(fake->IsClassSet("session-swipe-shown"));
  Drag(380, 140, kContactY);
  EXPECT_FALSE(contact_row_->IsClassSet("session-swipe-shown") && !chat_actions_.empty());
  EXPECT_TRUE(chat_actions_.empty()) << "the Chats gesture never sees a contact row";
}

TEST_F(NewChatSwipeHitTest, AttachingAndDetachingTheSecondInstanceLeavesTheChatsListAsItWas) {
  const std::vector<std::string> before = ChatsBehaviour();
  ASSERT_GE(before.size(), 4u);
  EXPECT_EQ(before[0], "t1:" + std::to_string(static_cast<int>(SessionSwipeAction::Delete)));
  AttachContacts();
  Drag(380, 140, kContactY);
  contacts_.Detach();
  EXPECT_FALSE(contacts_.Attached());
  EXPECT_FALSE(contact_row_->IsClassSet("session-swipe-shown")) << "released: the row is put back";
  EXPECT_EQ(ChatsBehaviour(), before);
}

// --- the controller side -------------------------------------------------------------------------------------------

// A result pill that names the person: the words before the name (the title shown depends on what other tests left).
std::string BeforeName(const std::string& key) {
  const std::string marked = Tr(key, {{"name", "\x01"}});
  return marked.substr(0, marked.find('\x01'));
}

struct FakeShell {
  std::vector<std::string> transients;
  ShellNavigationPorts Ports() {
    ShellNavigationPorts ports;
    ports.snapshot = []() {
      ShellChromeSnapshot snap;
      snap.layout_mode = LayoutMode::Compact;
      snap.nav_tab = NavTab::Sessions;
      return snap;
    };
    ports.push_transient = [this](const PaneSpec& spec) { transients.push_back(spec.key); };
    ports.pop_transient = []() {};
    return ports;
  }
};

class NewChatSwipeControllerTest : public ::testing::Test {
protected:
  void SetUp() override {
    Add("c-ann", "account:ann", TrustLevel::Unknown);
    Add("c-bob", "account:bob", TrustLevel::Unknown);
    Add("c-eve", "account:eve", TrustLevel::Blocked);
    MessagingContactsPorts ports;
    ports.snapshot = []() {
      MessagingView view;
      view.initialized = true;
      return view;
    };
    ports.get_contact = [this](const std::string& id) -> Roe<std::optional<Contact>> {
      const auto it = contacts_.find(id);
      return it == contacts_.end() ? std::optional<Contact>{} : std::optional<Contact>{it->second};
    };
    ports.list_contacts = [this]() -> Roe<std::vector<Contact>> {
      std::vector<Contact> all;
      for (const auto& [id, contact] : contacts_) {
        all.push_back(contact);
      }
      return all;
    };
    ports.upsert_contact = [this](const Contact& contact) -> Roe<Contact> {
      contacts_[contact.id] = contact;
      return contact;
    };
    ports.remove_contact = [this](const std::string& id) -> Roe<bool> { return contacts_.erase(id) > 0; };
    ports.find_or_create_direct_thread = [](const std::string& contact_id, ThreadChannel) -> Roe<Thread> {
      Thread thread;
      thread.id = "thread-of-" + contact_id;
      return thread;
    };
    controller_.BindContactsPorts(ports);
    controller_.BindShellNavigation(shell_.Ports());
    CallActionsPorts calls;
    calls.start_call = [this](const std::string& thread_id, const bool video) {
      started_.push_back({thread_id, video});
      return true;
    };
    calls.peer_calls_ready = []() { return true; };
    controller_.BindCallActions(calls);
    ShellFeedbackPorts feedback;
    feedback.show_toast = [this](const std::string& message, ToastDuration) { toasts_.push_back(message); };
    const auto yes = [](const std::string&, const std::string&, std::function<void(bool)> on_result,
                        const std::string&) { on_result(true); };
    feedback.show_confirm = yes;
    feedback.show_confirm_destructive = yes;
    controller_.BindShellFeedback(feedback);
    ContactsController::InstallInstance(controller_); // the block confirm reaches its controller through Instance()
    controller_.OpenNewChatPage();
  }

  void TearDown() override { ContactsController::ClearInstance(); }

  void Add(const std::string& id, const std::string& account, TrustLevel trust) {
    Contact contact;
    contact.id = id;
    contact.display_name = id;
    contact.remote.ids = {{mesh::ContactIdKind::Account, account, true}};
    contact.local.trust = trust;
    SyncContactMirrors(contact);
    contacts_[id] = contact;
  }

  static std::vector<std::string> Ids(const std::vector<ContextMenuAction>& actions) {
    std::vector<std::string> ids;
    for (const ContextMenuAction& action : actions) {
      ids.push_back(action.id);
    }
    return ids;
  }

  FakeShell shell_;
  std::map<std::string, Contact> contacts_;
  ContactsController controller_;
  std::vector<std::pair<std::string, bool>> started_;
  std::vector<std::string> toasts_;
};

TEST_F(NewChatSwipeControllerTest, VoiceAndVideoCallTheSwipedRowAndInfoOpensItsPage) {
  controller_.OnNewChatSwipeAction("c-bob", "voice");
  controller_.OnNewChatSwipeAction("c-ann", "video");
  ASSERT_EQ(started_.size(), 2u);
  EXPECT_EQ(started_[0], (std::pair<std::string, bool>{"thread-of-c-bob", false}));
  EXPECT_EQ(started_[1], (std::pair<std::string, bool>{"thread-of-c-ann", true}));
  controller_.OnNewChatSwipeAction("c-ann", "info");
  EXPECT_EQ(shell_.transients.back(), "contact_detail");
}

TEST_F(NewChatSwipeControllerTest, ABlockedContactsCallButtonsOnlyTellWhy) {
  controller_.OnNewChatSwipeAction("c-eve", "voice");
  controller_.OnNewChatSwipeAction("c-eve", "video");
  EXPECT_TRUE(started_.empty());
  ASSERT_EQ(toasts_.size(), 2u);
  EXPECT_EQ(toasts_[0], Tr("contacts.call_blocked"));
}

TEST_F(NewChatSwipeControllerTest, MoreOffersBlockAndRemoveOrUnblockAndRemove) {
  const auto normal = controller_.NewChatMoreActions("c-ann");
  ASSERT_EQ(Ids(normal), (std::vector<std::string>{"block", "remove"}));
  EXPECT_TRUE(normal[0].danger);
  EXPECT_TRUE(normal[1].danger);
  const auto blocked = controller_.NewChatMoreActions("c-eve");
  ASSERT_EQ(Ids(blocked), (std::vector<std::string>{"unblock", "remove"}));
  EXPECT_FALSE(blocked[0].danger) << "unblocking is not red";
  EXPECT_TRUE(blocked[1].danger);
  EXPECT_TRUE(controller_.NewChatMoreActions("c-nobody").empty());
}

TEST_F(NewChatSwipeControllerTest, BlockingFromMoreSaysSoAndRefreshesTheList) {
  const auto rows_before = controller_.NewChatRowIds();
  ASSERT_NE(std::find(rows_before.begin(), rows_before.end(), "c-ann"), rows_before.end());
  controller_.OnSelectContact("c-bob"); // another page was open: the action still works on Ann
  auto actions = controller_.NewChatMoreActions("c-ann");
  ASSERT_FALSE(actions.empty());
  toasts_.clear();
  actions[0].run();
  EXPECT_EQ(contacts_["c-ann"].local.trust, TrustLevel::Blocked);
  EXPECT_EQ(contacts_["c-bob"].local.trust, TrustLevel::Unknown) << "the open page's contact is untouched";
  ASSERT_FALSE(toasts_.empty());
  EXPECT_EQ(toasts_.back().rfind(BeforeName("contacts.toast.blocked"), 0), 0u) << toasts_.back();
  const auto rows_after = controller_.NewChatRowIds();
  EXPECT_NE(rows_after, rows_before) << "the list was read again";
}

TEST_F(NewChatSwipeControllerTest, UnblockingFromMoreSaysSo) {
  auto actions = controller_.NewChatMoreActions("c-eve");
  ASSERT_FALSE(actions.empty());
  toasts_.clear();
  actions[0].run();
  EXPECT_EQ(contacts_["c-eve"].local.trust, TrustLevel::Unknown);
  ASSERT_FALSE(toasts_.empty());
  EXPECT_EQ(toasts_.back().rfind(BeforeName("contacts.toast.unblocked"), 0), 0u) << toasts_.back();
}

TEST_F(NewChatSwipeControllerTest, RemovingFromMoreDropsTheRowAtOnce) {
  auto actions = controller_.NewChatMoreActions("c-bob");
  ASSERT_EQ(actions.size(), 2u);
  actions[1].run();
  EXPECT_EQ(contacts_.count("c-bob"), 0u);
  const auto rows = controller_.NewChatRowIds();
  EXPECT_EQ(std::find(rows.begin(), rows.end(), "c-bob"), rows.end());
  ASSERT_FALSE(toasts_.empty());
  EXPECT_EQ(toasts_.back(), Tr("contacts.toast.removed"));
}

} // namespace
} // namespace pbr
