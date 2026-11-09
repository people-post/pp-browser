// DEV/PLAN-minimal-nav.md B / C: the phone's AI conversation list is a drawer from the left on the AI page. Its open
// paths, its close paths and resets, the drag that opens it, and (with the real view mounted) its rows' swipes.
#include "common/runtime/AppRuntime.h"
#include "foundation/platform/IAssetLocator.h"
#include "gui/CallChromeSync.h"
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/ChatScreenState.h"
#include "gui/shell/DataModelHost.h"
#include "gui/shell/ShellCallChromePorts.h"
#include "gui/shell/ShellDrawerOpenGesture.h"
#include "gui/shell/ShellHost.h"
#include "gui/shell/ShellLayoutRml.h"
#include "lib/ui/DataEvent.h"
#include "lib/ui/ShellLayout.h"
#include "lib/ui/ViewCatalog.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/Event.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pbr {
namespace {

bool Has(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

// RequestSyncLayout / RemountAiDrawer queue UI tasks that hold the host; kept to the end of the process (see
// shell_host_test.cpp).
ShellHost& KeptHost() {
  static std::vector<std::unique_ptr<ShellHost>> hosts;
  return *hosts.emplace_back(std::make_unique<ShellHost>());
}

ShellHost& PhoneHost(const NavTab tab = NavTab::Home) {
  ShellHost& host = KeptHost();
  host.State().layout_mode = LayoutMode::Compact;
  ShellLayout::SyncLayoutModeString(host.State());
  host.SelectNavTab(tab);
  return host;
}

ShellHost& PhoneHostWithDrawerOpen() {
  ShellHost& host = PhoneHost();
  host.OpenAiDrawer();
  EXPECT_TRUE(host.State().ai_drawer_open);
  return host;
}

// --- open / close / resets (state) ------------------------------------------------------------------------------

TEST(AiDrawerTest, OpensAndCloses) {
  ShellHost& host = PhoneHost();
  EXPECT_TRUE(host.HomeRootShown());
  host.OpenAiDrawer();
  EXPECT_TRUE(host.State().ai_drawer_open);
  EXPECT_FALSE(host.HomeRootShown()) << "the drawer is over the AI page";
  host.OpenAiDrawer();
  EXPECT_TRUE(host.State().ai_drawer_open);
  host.CloseAiDrawer();
  EXPECT_FALSE(host.State().ai_drawer_open);
  EXPECT_TRUE(host.HomeRootShown());
}

TEST(AiDrawerTest, ASecondTapOnAiDoesNotOpenIt) {
  ShellHost& host = PhoneHost(NavTab::Sessions);
  host.OnNavTabClicked("home");
  EXPECT_FALSE(host.State().ai_drawer_open) << "the first tap goes to the AI page";
  host.OnNavTabClicked("home");
  EXPECT_FALSE(host.State().ai_drawer_open) << "a second tap stays on the AI page";
  host.OpenAiDrawer();
  EXPECT_TRUE(host.State().ai_drawer_open);
}

TEST(AiDrawerTest, ResetsOnATabPick) {
  for (const NavTab tab : {NavTab::Home, NavTab::Sessions, NavTab::Contacts, NavTab::Me}) {
    ShellHost& host = PhoneHostWithDrawerOpen();
    host.SelectNavTab(tab);
    EXPECT_FALSE(host.State().ai_drawer_open) << ShellLayout::NavTabString(tab);
  }
}

TEST(AiDrawerTest, ResetsWhenAChatOverlayOpens) {
  ShellHost& host = PhoneHostWithDrawerOpen();
  host.RegisterPane({.key = "chat", .rml_path = "views/chat.rml", .provides_composer = true});
  host.SetPrimaryPane("chat");
  host.OpenCompactChat();
  EXPECT_FALSE(host.State().ai_drawer_open);
  EXPECT_TRUE(host.State().compact_chat_open);
}

TEST(AiDrawerTest, ResetsWhenAPageOrALayerIsPushed) {
  ShellHost& pushed = PhoneHostWithDrawerOpen();
  pushed.PushTransient({.key = "contact_detail"});
  EXPECT_FALSE(pushed.State().ai_drawer_open) << "a pushed page";

  ShellHost& layered = PhoneHostWithDrawerOpen();
  layered.PushLayer({.key = "new_chat", .page_sheet = true});
  EXPECT_FALSE(layered.State().ai_drawer_open) << "a layer";
}

TEST(AiDrawerTest, ResetsWhenARingOrTheCallScreenRises) {
  ShellHost& ring = PhoneHostWithDrawerOpen();
  CallChromeSnapshot ringing;
  ringing.ring.active = true;
  ring.ApplyCallChromeSnapshot(ringing, CallChromeUpdate::Remount);
  EXPECT_FALSE(ring.State().ai_drawer_open) << "an incoming call";

  ShellHost& call = PhoneHostWithDrawerOpen();
  CallChromeSnapshot screen;
  screen.in_progress.active = true;
  screen.in_progress.mode = CallChromeMode::Expanded;
  call.ApplyCallChromeSnapshot(screen, CallChromeUpdate::Remount);
  EXPECT_FALSE(call.State().ai_drawer_open) << "the call screen";

  ShellHost& chip = PhoneHostWithDrawerOpen();
  CallChromeSnapshot minimised;
  minimised.in_progress.active = true;
  minimised.in_progress.mode = CallChromeMode::Minimized;
  chip.ApplyCallChromeSnapshot(minimised, CallChromeUpdate::Remount);
  EXPECT_TRUE(chip.State().ai_drawer_open) << "the minimised call chip leaves the page usable";
}

TEST(AiDrawerTest, TheDesktopNeverHasIt) {
  ShellHost& desktop = KeptHost();
  desktop.SelectNavTab(NavTab::Home);
  ASSERT_EQ(desktop.State().layout_mode, LayoutMode::Expanded);
  desktop.OpenAiDrawer();
  EXPECT_FALSE(desktop.State().ai_drawer_open);
  desktop.OnNavTabClicked("home");
  EXPECT_FALSE(desktop.State().ai_drawer_open);

  ShellState state;
  state.layout_mode = LayoutMode::Expanded;
  state.nav_tab = NavTab::Home;
  state.ai_drawer_open = true;  // even if set
  EXPECT_TRUE(ShellLayoutRml(state, false, "").AiDrawer().empty());
  EXPECT_FALSE(Has(ShellLayoutRml(state, false, "").ShellRoot(), "shell-drawer-layer"));
}

TEST(AiDrawerTest, TheSystemBackClosesTheDrawerAndNothingElse) {
  ShellHost& host = PhoneHostWithDrawerOpen();
  bool other_back = false;
  host.PushLocalBack("some_page", [&other_back]() { other_back = true; });
  EXPECT_TRUE(host.HandleDismiss());
  EXPECT_FALSE(host.State().ai_drawer_open);
  EXPECT_FALSE(other_back) << "only the drawer";
  EXPECT_EQ(host.State().nav_tab, NavTab::Home);
  EXPECT_TRUE(host.HandleDismiss());
  EXPECT_TRUE(other_back) << "the next back goes on down the order";
  EXPECT_FALSE(host.HandleDismiss()) << "then the platform's (minimise)";
}

// --- markup ---------------------------------------------------------------------------------------------------

TEST(AiDrawerTest, TheLayerIsAboveTheTabBarWithAScrimThatCloses) {
  ShellState state;
  state.layout_mode = LayoutMode::Compact;
  state.nav_tab = NavTab::Home;
  EXPECT_TRUE(ShellLayoutRml(state, false, "").AiDrawer().empty()) << "closed";
  state.ai_drawer_open = true;
  const std::string layer = ShellLayoutRml(state, false, "").AiDrawer();
  EXPECT_TRUE(Has(layer, "<div class=\"shell-layer shell-layer-compact shell-drawer-layer\" data-model=\"window\">"));
  EXPECT_TRUE(Has(layer, "<div class=\"shell-scrim shell-drawer-scrim\" data-event-click=\"close_ai_drawer()\">"));
  EXPECT_TRUE(Has(layer, "id=\"shell-ai-drawer-mount\""));
  const std::string root = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(root, "id=\"shell-ai-drawer-host\"")) << "the drawer's own host, filled apart from the root";
  EXPECT_TRUE(Has(root, "id=\"shell-bottom-chrome\"")) << "the tab bar stays, under the scrim";
  EXPECT_EQ(ShellLayout::CompactTabBarDp(state), 48.f);
}

TEST(AiDrawerTest, AnAiConversationsBarHasANewConversationButton) {
  ShellState state;
  state.layout_mode = LayoutMode::Compact;
  state.nav_tab = NavTab::Home;
  state.compact_chat_open = true;
  const std::string rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(rml, "<div class=\"shell-chat-new-slot\" data-model=\"chat\" data-if=\"thread_is_ai\">"));
  EXPECT_TRUE(Has(rml, "data-model=\"shell\" data-event-click=\"new_chat()\""));
}

// --- the drag that opens it -------------------------------------------------------------------------------------

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

class ContextTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer_);
    initialized_ = ui::Initialise();
    ASSERT_TRUE(initialized_);
    context_ = ui::CreateContext("ai-drawer-test", {400, 800});
    ASSERT_NE(context_, nullptr);
  }

  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context_) {
      ui::RemoveContext("ai-drawer-test");
    }
    if (initialized_) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }

  void Press(const int x, const int y) {
    context_->ProcessMouseMove(x, y, 0);
    context_->ProcessMouseButtonDown(0, 0);
  }

  void Drag(const int from_x, const int from_y, const int to_x, const int to_y) {
    Press(from_x, from_y);
    const int steps = 10;
    for (int i = 1; i <= steps; ++i) {
      context_->ProcessMouseMove(from_x + (to_x - from_x) * i / steps, from_y + (to_y - from_y) * i / steps, 0);
      context_->Update();
    }
    context_->ProcessMouseButtonUp(0, 0);
    context_->Update();
  }

  void Tap(ui::Element* element) {
    ASSERT_NE(element, nullptr);
    const ui::Vector2f at = element->GetAbsoluteOffset(ui::BoxArea::Border) +
                            element->GetBox().GetSize(ui::BoxArea::Border) * 0.5f;
    context_->ProcessMouseMove(static_cast<int>(at.x), static_cast<int>(at.y), 0);
    context_->ProcessMouseButtonDown(0, 0);
    context_->ProcessMouseButtonUp(0, 0);
    context_->Update();
  }

  NullRender renderer_;
  bool initialized_ = false;
  ui::Context* context_ = nullptr;
};

constexpr const char* kPageRml = R"(<rml><head><style>
body { display: block; width: 400px; height: 800px; margin: 0; }
div, p, textarea, button { display: block; }
#page { width: 400px; height: 800px; }
#home-composer-mount { height: 80px; }
.home-suggestions { height: 40px; }
#plain { height: 400px; }
</style></head><body><div id="page">
<div id="home-composer-mount"><div class="prompt-composer" style="height: 60px; width: 380px;"></div></div>
<div class="home-suggestions"><button id="chip" style="height: 32px; width: 100px;"></button></div>
<div id="plain"></div>
</div></body></rml>)";

class DrawerOpenGestureTest : public ContextTest {
protected:
  void SetUp() override {
    ContextTest::SetUp();
    document_ = context_->LoadDocumentFromMemory(kPageRml);
    ASSERT_NE(document_, nullptr);
    document_->Show();
    context_->Update();
    gesture_.Attach(document_->GetElementById("page"), context_, {}, [this]() { return can_open_; },
                    [this]() { ++opened_; });
  }

  void TearDown() override {
    gesture_.Detach();
    ContextTest::TearDown();
  }

  ui::ElementDocument* document_ = nullptr;
  ShellDrawerOpenGesture gesture_;
  bool can_open_ = true;
  int opened_ = 0;
};

// The plain area of the page (the composer is 0-80, the chips 80-120).
constexpr int kPlainY = 300;

TEST(DrawerOpenRule, MoreThanSixtyDpToTheRightAndMoreSidewaysThanNot) {
  EXPECT_FALSE(ShellDrawerOpenGesture::Opens(59.f, 0.f));
  EXPECT_FALSE(ShellDrawerOpenGesture::Opens(60.f, 0.f));
  EXPECT_TRUE(ShellDrawerOpenGesture::Opens(61.f, 0.f));
  EXPECT_TRUE(ShellDrawerOpenGesture::Opens(80.f, -40.f));
  EXPECT_FALSE(ShellDrawerOpenGesture::Opens(80.f, 80.f)) << "as much up or down as across";
  EXPECT_FALSE(ShellDrawerOpenGesture::Opens(-120.f, 0.f)) << "leftward";
}

TEST_F(DrawerOpenGestureTest, FiftyNineDpDoesNotOpenSixtyOneDoes) {
  ASSERT_EQ(context_->GetDensityIndependentPixelRatio(), 1.f);
  Drag(50, kPlainY, 109, kPlainY);
  EXPECT_EQ(opened_, 0) << "59dp";
  Drag(50, kPlainY, 111, kPlainY);
  EXPECT_EQ(opened_, 1) << "61dp";
}

TEST_F(DrawerOpenGestureTest, AMostlyVerticalDragDoesNotOpen) {
  Drag(50, 200, 130, 600);
  EXPECT_EQ(opened_, 0);
  Drag(250, kPlainY, 50, kPlainY);
  EXPECT_EQ(opened_, 0) << "leftward";
}

TEST_F(DrawerOpenGestureTest, NotWhenThePageSaysNo) {
  can_open_ = false;  // the drawer already open, or a chat overlay over the AI page (ShellHost::HomeRootShown)
  Drag(50, kPlainY, 200, kPlainY);
  EXPECT_EQ(opened_, 0);
}

TEST_F(DrawerOpenGestureTest, NotFromTheComposerOrTheChips) {
  Drag(20, 30, 200, 30);
  EXPECT_EQ(opened_, 0) << "the composer";
  Drag(20, 96, 200, 96);
  EXPECT_EQ(opened_, 0) << "the chips";
  Drag(20, kPlainY, 200, kPlainY);
  EXPECT_EQ(opened_, 1) << "the page itself";
}

namespace {
class CountClicks : public ui::EventListener {
public:
  void ProcessEvent(ui::Event&) override { ++count; }
  int count = 0;
};
}  // namespace

// The click that ends the drag which opened the drawer is swallowed however long the drag took, and only that one.
TEST_F(DrawerOpenGestureTest, ASlowDragStillSwallowsItsOwnClickAndOnlyThat) {
  ui::Element* plain = document_->GetElementById("plain");
  ASSERT_NE(plain, nullptr);
  CountClicks clicks;
  plain->AddEventListener(ui::EventId::Click, &clicks);

  Press(50, kPlainY);
  for (int i = 1; i <= 10; ++i) {
    context_->ProcessMouseMove(50 + 8 * i, kPlainY, 0);
    context_->Update();
  }
  ASSERT_EQ(opened_, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(400));  // the finger stays down well past the old window
  context_->ProcessMouseButtonUp(0, 0);
  context_->Update();
  EXPECT_EQ(clicks.count, 0) << "the drag's own click is swallowed";

  Tap(plain);
  EXPECT_EQ(clicks.count, 1) << "a later, unrelated click is not";
  plain->RemoveEventListener(ui::EventId::Click, &clicks);
}

TEST(AiDrawerTest, TheDragIsOfferedOnlyOnTheBareAiPage) {
  // ShellHost asks HomeRootShown (and that the page is the minimal one) before the drag may open the drawer.
  ShellHost& open = PhoneHostWithDrawerOpen();
  EXPECT_FALSE(open.HomeRootShown()) << "already open";
  ShellHost& chat = PhoneHost();
  chat.RegisterPane({.key = "chat", .rml_path = "views/chat.rml", .provides_composer = true});
  chat.OpenCompactChat();
  EXPECT_FALSE(chat.HomeRootShown()) << "a chat over the AI page";
  EXPECT_FALSE(PhoneHost(NavTab::Sessions).HomeRootShown()) << "not the AI page";
}

// --- the real shell and the real ai_drawer view -------------------------------------------------------------------

class AssetsLocator : public IAssetLocator {
public:
  std::string Resolve(const std::string& relative) const override {
    return (std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative).string();
  }
};

// The shell's geometry, enough for the hit tests: the drawer 86% wide, 60px rows whose swipe buttons sit outside them.
constexpr const char* kShellRml = R"(<rml><head><style>
body { display: block; width: 400px; height: 800px; margin: 0; }
div, p, button, span { display: block; }
#shell-root { position: absolute; top: 0; left: 0; width: 400px; height: 800px; }
.shell-layer { position: absolute; top: 0; left: 0; right: 0; bottom: 0; }
.shell-drawer-host { position: absolute; top: 0; left: 0; right: 0; bottom: 0; z-index: 28; pointer-events: none; }
.shell-drawer-host .shell-layer { pointer-events: auto; }
.shell-scrim { position: absolute; top: 0; left: 0; right: 0; bottom: 0; }
.shell-drawer { position: absolute; top: 0; left: 0; bottom: 0; width: 86%; }
.shell-drawer-body { position: absolute; top: 0; left: 0; right: 0; bottom: 0; }
.ai-drawer-head { height: 44px; }
#ai-drawer-sessions { height: 600px; overflow-x: hidden; }
.sidebar-session { position: relative; height: 60px; }
.session-swipe { display: none; }
.sidebar-session.session-swipe-shown .session-swipe { display: flex; flex-direction: row; position: absolute; top: 0; bottom: 0; }
.session-swipe--end { left: 100%; }
.session-swipe--start { right: 100%; }
.session-swipe-btn { width: 74px; height: 60px; }
#home-landing { height: 500px; }
</style></head><body><input id="probe" type="text" style="width: 100px; height: 20px;"/>
<div id="shell-root"></div><div id="shell-statusbar-strip" class="shell-statusbar-strip"></div>
<div id="shell-home-indicator-strip" class="shell-home-indicator-strip"></div></body></rml>)";

SessionDisplayRow AiRow(const char* id, const char* title) {
  SessionDisplayRow row;
  row.id = id;
  row.title = title;
  row.kind = "ai";
  return row;
}

class ShellDrawerTest : public ContextTest {
protected:
  void SetUp() override {
    ContextTest::SetUp();
    // A fresh UI queue: tasks other tests left behind hold objects that are gone.
    AppRuntime::ShutdownUI();
    AppRuntime::InitializeUI();
    IAssetLocator::SetInstance(&locator_);
    ViewCatalog::ClearCache();
    host_.Initialize(context_);
    ASSERT_TRUE(host_.RegisterWindowModel(context_));
    shell_.ai_sessions = {AiRow("ai1", "First"), AiRow("ai2", "Second")};
    ASSERT_TRUE(DataModelHost::Instance().Register(context_, "shell", [this](ui::DataModelConstructor& ctor) {
      BindChatShellState(ctor, shell_);
      // As ChatController: an AI conversation picked opens the chat overlay over the AI page.
      ctor.BindEventCallback("select_thread", DataEvent<std::string>([this](const std::string& id) {
        selected_ = id;
        host_.SetPrimaryPane("chat");
        host_.OpenCompactChat();
        attached_after_pick_ = host_.AiDrawerRowSwipesAttached();
      }));
      ctor.BindEventCallback("toggle_archived_ai", DataEvent([] {}));
      ctor.BindEventCallback("new_chat", DataEvent([] {}));
    }));
    host_.SetOnSessionSwipeAction([this](const std::string& thread_id, const SessionSwipeAction action) {
      actions_.push_back(thread_id + ":" + std::to_string(static_cast<int>(action)));
    });
    document_ = context_->LoadDocumentFromMemory(kShellRml);
    ASSERT_NE(document_, nullptr);
    document_->Show();
    host_.RegisterPane({.key = "chat", .rml_path = "views/chat.rml", .provides_composer = true});
    host_.State().layout_mode = LayoutMode::Compact;
    ShellLayout::SyncLayoutModeString(host_.State());
    host_.SelectNavTab(NavTab::Home);
    host_.SyncLayout();
    Settle();
  }

  void TearDown() override {
    AppRuntime::ShutdownUI();
    AppRuntime::InitializeUI();
    IAssetLocator::SetInstance(nullptr);
    ViewCatalog::ClearCache();
    ContextTest::TearDown();
  }

  /** The next UI turns (the deferred remounts), then a layout pass. */
  void Settle() {
    for (int i = 0; i < 3; ++i) {
      AppRuntime::RunUITasks();
      context_->Update();
    }
  }

  ui::Element* ById(const char* id) const { return document_->GetElementById(id); }

  ui::Element* Row(const char* session_id) const {
    ui::Element* list = ById("ai-drawer-sessions");
    if (!list) {
      return nullptr;
    }
    ui::ElementList rows;
    list->GetElementsByClassName(rows, "sidebar-session");
    for (ui::Element* row : rows) {
      if (row->GetAttribute("session-id", ui::String()) == session_id) {
        return row;
      }
    }
    return nullptr;
  }

  AssetsLocator locator_;
  ShellHost host_;
  ChatShellState shell_;
  ui::ElementDocument* document_ = nullptr;
  std::string selected_;
  bool attached_after_pick_ = true;
  std::vector<std::string> actions_;
};

TEST_F(ShellDrawerTest, TheRealViewMountsAndItsRowsSwipe) {
  EXPECT_EQ(ById("shell-ai-drawer-mount"), nullptr) << "closed: nothing in the host";
  host_.OpenAiDrawer();
  Settle();
  ASSERT_NE(ById("shell-ai-drawer-mount"), nullptr);
  ASSERT_NE(ById("ai-drawer-sessions"), nullptr) << "the ai_drawer view is mounted";
  EXPECT_TRUE(host_.AiDrawerRowSwipesAttached());
  ui::Element* row = Row("ai1");
  ASSERT_NE(row, nullptr);
  ASSERT_NE(Row("ai2"), nullptr);

  // A left swipe on the first row shows its actions; More gets its tap (ChatController opens the action sheet).
  const ui::Vector2f top_left = row->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Vector2f size = row->GetBox().GetSize(ui::BoxArea::Border);
  const int y = static_cast<int>(top_left.y + size.y * 0.5f);
  const int right = static_cast<int>(top_left.x + size.x);
  Drag(right - 14, y, right - 14 - 222, y);
  ASSERT_TRUE(row->IsClassSet("session-swipe-shown"));
  ui::ElementList more;
  row->QuerySelectorAll(more, "[swipe-action=more]");
  ASSERT_EQ(more.size(), 1u);
  Tap(more.front());
  EXPECT_EQ(actions_, std::vector<std::string>{"ai1:" + std::to_string(static_cast<int>(SessionSwipeAction::More))});
}

// The "you" row carries the device's link status under the nickname (ShellHost::SetChatsLink): nothing while unknown,
// the teal cloud-tick icon and its word when online, the red cloud-slash icon and its word when offline.
TEST_F(ShellDrawerTest, TheMeRowShowsTheLinkStatusUnderTheName) {
  host_.OpenAiDrawer();
  Settle();
  ui::Element* line = ById("ai-drawer-me")->QuerySelector(".ai-drawer-me-link");
  ASSERT_NE(line, nullptr);
  EXPECT_FALSE(line->IsVisible()) << "unknown state shows nothing";

  auto count_visible = [&](const char* cls) {
    ui::ElementList found;
    ById("ai-drawer-me")->GetElementsByClassName(found, cls);
    int n = 0;
    for (ui::Element* e : found) {
      n += e->IsVisible() ? 1 : 0;
    }
    return n;
  };

  host_.SetChatsLink(true, true);
  Settle();
  EXPECT_TRUE(line->IsVisible());
  EXPECT_EQ(count_visible("ai-drawer-me-link-icon--ok"), 1);
  EXPECT_EQ(count_visible("ai-drawer-me-link-icon--offline"), 0);
  EXPECT_FALSE(host_.State().chats_link_text.empty());
  EXPECT_NE(line->GetInnerRML().find(host_.State().chats_link_text), std::string::npos) << "the word is rendered";

  host_.SetChatsLink(true, false);
  Settle();
  EXPECT_TRUE(line->IsVisible());
  EXPECT_EQ(count_visible("ai-drawer-me-link-icon--ok"), 0);
  EXPECT_EQ(count_visible("ai-drawer-me-link-icon--offline"), 1);
  EXPECT_NE(line->GetInnerRML().find(host_.State().chats_link_text), std::string::npos);

  host_.SetChatsLink(ChatsLink::Connecting);
  Settle();
  EXPECT_TRUE(line->IsVisible());
  EXPECT_EQ(host_.State().chats_link_state, "connecting");
  EXPECT_EQ(count_visible("ai-drawer-me-link-icon--ok"), 0);
  EXPECT_EQ(count_visible("ai-drawer-me-link-icon--offline"), 0);
  EXPECT_EQ(count_visible("ai-drawer-me-link-icon--connecting"), 1);
  EXPECT_NE(line->GetInnerRML().find(host_.State().chats_link_text), std::string::npos);

  host_.SetChatsLink(false, false);
  Settle();
  EXPECT_FALSE(line->IsVisible());
}

TEST_F(ShellDrawerTest, APickedConversationClosesTheDrawerAndOpensOverTheAiPage) {
  host_.OpenAiDrawer();
  Settle();
  ASSERT_TRUE(host_.AiDrawerRowSwipesAttached());
  Tap(Row("ai2"));
  EXPECT_EQ(selected_, "ai2");
  EXPECT_FALSE(attached_after_pick_) << "the rows' swipes are let go inside the pick";
  EXPECT_FALSE(host_.State().ai_drawer_open);
  EXPECT_EQ(host_.State().nav_tab, NavTab::Home) << "an AI conversation stays on the AI page";
  Settle();
  EXPECT_EQ(ById("ai-drawer-sessions"), nullptr) << "the drawer is gone";
  EXPECT_NE(ById("shell-chat-overlay"), nullptr) << "the conversation is open";
  EXPECT_FALSE(host_.AiDrawerRowSwipesAttached());
}

TEST_F(ShellDrawerTest, OpeningLetsGoOfTheKeyboard) {
  ui::Element* probe = ById("probe");
  ASSERT_NE(probe, nullptr);
  probe->Focus();
  host_.State().safe_area_bottom_dp = 300;  // the keyboard is up
  ASSERT_EQ(context_->GetFocusElement(), probe);
  host_.OpenAiDrawer();
  EXPECT_NE(context_->GetFocusElement(), probe) << "the field let go";
  EXPECT_EQ(host_.State().safe_area_bottom_dp, 0) << "the keyboard's inset is dropped";
  EXPECT_TRUE(host_.State().ai_drawer_open);
}

TEST_F(ShellDrawerTest, ADragAcrossTheAiPageOpensIt) {
  ui::Element* landing = ById("home-landing");
  ASSERT_NE(landing, nullptr) << "home.rml's minimal block";
  ASSERT_TRUE(landing->IsClassSet("home-landing--minimal"));
  const ui::Vector2f at = landing->GetAbsoluteOffset(ui::BoxArea::Border);
  // Below the composer and the chips, at the bottom of the block.
  const int y = static_cast<int>(at.y + landing->GetBox().GetSize(ui::BoxArea::Border).y - 10.f);
  Drag(30, y, 130, y);
  EXPECT_TRUE(host_.State().ai_drawer_open);
  Settle();
  EXPECT_NE(ById("ai-drawer-sessions"), nullptr);
}

// The phone's status-bar strip takes the colour of the page under it: one class per surface (colors.rcss.tmpl), so the
// top inset does not end in a line over the grouped Me page or the full-screen New chat / Add contact pages.
TEST_F(ShellDrawerTest, TheStatusStripNamesThePageUnderIt) {
  ui::Element* strip = ById("shell-statusbar-strip");
  ASSERT_NE(strip, nullptr);
  const auto only = [strip](const char* surface) {
    for (const char* known : {"home", "me", "new_chat", "add_contact"}) {
      if (strip->IsClassSet(std::string("shell-statusbar-strip--") + known) != (std::string(known) == surface)) {
        return false;
      }
    }
    return true;
  };
  EXPECT_TRUE(only("home"));
  host_.SelectNavTab(NavTab::Me);
  host_.SyncLayout();
  EXPECT_TRUE(only("me"));
  host_.SelectNavTab(NavTab::Sessions);
  host_.SyncLayout();
  EXPECT_TRUE(only("")) << "the Chats bar keeps the body colour";
  host_.PushTransient({.key = "new_chat"});
  host_.SyncLayout();
  EXPECT_TRUE(only("new_chat"));
  host_.PushTransient({.key = "add_contact"});
  host_.SyncLayout();
  EXPECT_TRUE(only("add_contact"));
}

// The band over the home indicator: under a page with no bottom bar (Me, its detail pages, a pushed page) it is the
// inset tall and named like the top strip, so the grouped pages do not end in a white band; under the tab bar it is 0.
TEST_F(ShellDrawerTest, TheHomeIndicatorStripCoversTheInsetUnderGroupedPages) {
  ui::Element* bottom = ById("shell-home-indicator-strip");
  ASSERT_NE(bottom, nullptr);
  host_.SetSafeAreaInsetsFromPrefs(59, 34);
  const auto height = [bottom]() { return bottom->GetProperty<float>("height"); };
  host_.SyncLayout();
  EXPECT_EQ(height(), 0.f) << "Home: the tab bar owns the inset";
  host_.SelectNavTab(NavTab::Me);
  host_.SyncLayout();
  EXPECT_EQ(height(), 0.f) << "Settings keeps the tab bar (TODO #200): it owns the inset";
  EXPECT_TRUE(bottom->IsClassSet("shell-home-indicator-strip--me"));
  // A settings detail page (安全, 网络…) is Me's inline detail with a local back, not a pushed page: still Me.
  host_.PushLocalBack("settings_detail", [] {});
  host_.SyncLayout();
  EXPECT_TRUE(bottom->IsClassSet("shell-home-indicator-strip--me"));
  EXPECT_TRUE(ById("shell-statusbar-strip")->IsClassSet("shell-statusbar-strip--me"));
  host_.ClearLocalBack("settings_detail");
  host_.SelectNavTab(NavTab::Sessions);
  host_.SyncLayout();
  EXPECT_EQ(height(), 0.f) << "Chats: the tab bar";
  host_.PushTransient({.key = "new_chat"});
  host_.SyncLayout();
  EXPECT_EQ(height(), 34.f);
  EXPECT_TRUE(bottom->IsClassSet("shell-home-indicator-strip--new_chat"));
}

// The keyboard / emoji sheet lifts #shell-root and, once gone, the lift is gone with it (the Home block re-centres in
// the page again): nothing on the way up is left behind on the way down.
TEST_F(ShellDrawerTest, TheShellRootLiftIsGoneWhenTheKeyboardOrEmojiSheetGoes) {
  ui::Element* root = ById("shell-root");
  ASSERT_NE(root, nullptr);
  const auto bottom = [root]() { return root->GetProperty<float>("bottom"); };
  const float rest = bottom();
  host_.State().safe_area_bottom_dp = 300;  // the keyboard is up
  host_.RefreshSafeAreaInsets(context_);    // the platform reports it gone
  Settle();
  EXPECT_EQ(host_.State().safe_area_bottom_dp, 0);
  EXPECT_EQ(bottom(), rest) << "keyboard: the lift is dropped";
  // (Not on the AI Home, which the sheet does not lift any more: TODO #204, HomeKeyboardDockTest.)
  host_.SelectNavTab(NavTab::Sessions);
  host_.SyncLayout();
  Settle();
  const float rest_chats = bottom();
  BottomChromeSpec spec;
  spec.key = "emoji_panel";
  host_.SetBottomChrome(spec);
  Settle();
  EXPECT_GT(bottom(), rest_chats) << "emoji sheet: lifted";
  host_.ClearBottomChrome();
  Settle();
  EXPECT_EQ(host_.State().safe_area_bottom_dp, 0);
  EXPECT_EQ(bottom(), rest_chats) << "emoji sheet gone: the lift is dropped";
}

// TODO #204 (real ShellHost): the AI Home does not follow the keyboard / emoji sheet. Its mascot and card stay where they
// are, the keyboard covers the chips; only when it would cover the card the block moves up by exactly the overlap
// (card bottom = keyboard top - 8). Every other page keeps the full lift.
class HomeKeyboardDockTest : public ShellDrawerTest {
protected:
  void SetUp() override {
    ShellDrawerTest::SetUp();
    ui::ElementList cards;
    ById("home-landing")->GetElementsByClassName(cards, "prompt-composer");
    ASSERT_FALSE(cards.empty()) << "the Home card is mounted";
    card_ = cards.front();
    mascot_ = ById("home-logo");
    ASSERT_NE(mascot_, nullptr);
    root_ = ById("shell-root");
    ASSERT_NE(root_, nullptr);
    rest_ = Snapshot();
  }
  struct Snap {
    float lift, card_bottom, card_top, logo_top, chips_top;
  };
  Snap Snapshot() {
    context_->Update();
    ui::ElementList chips;
    document_->GetElementsByClassName(chips, "chat-suggestion");
    return {root_->GetProperty<float>("bottom"), card_->GetAbsoluteOffset(ui::BoxArea::Border).y +
                card_->GetBox().GetSize(ui::BoxArea::Border).y,
            card_->GetAbsoluteOffset(ui::BoxArea::Border).y, mascot_->GetAbsoluteOffset(ui::BoxArea::Border).y,
            chips.empty() ? 0.f : chips.front()->GetAbsoluteOffset(ui::BoxArea::Border).y};
  }
  void KeyboardUp(int dp) {
    host_.SetSafeAreaInsetsFromPrefs(0, dp);
    Settle();
  }
  void KeyboardDown() {
    host_.SetSafeAreaInsetsFromPrefs(0, 0);
    host_.RefreshSafeAreaInsets(context_);
    Settle();
  }
  ui::Element* card_ = nullptr;
  ui::Element* mascot_ = nullptr;
  ui::Element* root_ = nullptr;
  Snap rest_{};
};

TEST_F(HomeKeyboardDockTest, ATallScreenKeepsTheBlockAndTheKeyboardCoversTheChips) {
  const int keyboard = static_cast<int>(800.f - rest_.card_bottom - 8.f - 20.f);  // card clear of it by 28dp
  ASSERT_GE(keyboard, 100);
  KeyboardUp(keyboard);
  const Snap up = Snapshot();
  std::printf("[ShellHost tall] keyboard %d (top %d) lift %.1f card %.1f..%.1f (rest %.1f..%.1f) logo %.1f (rest %.1f) chips %.1f (rest %.1f)\n",
              keyboard, 800 - keyboard, up.lift, up.card_top, up.card_bottom, rest_.card_top, rest_.card_bottom,
              up.logo_top, rest_.logo_top, up.chips_top, rest_.chips_top);
  EXPECT_EQ(up.lift, rest_.lift) << "the root is not lifted";
  EXPECT_FLOAT_EQ(up.card_top, rest_.card_top);
  EXPECT_FLOAT_EQ(up.logo_top, rest_.logo_top);
  EXPECT_FLOAT_EQ(up.chips_top, rest_.chips_top);
  EXPECT_GE(up.chips_top, up.card_bottom) << "the chips are under the card, where they were (the keyboard covers them)";
  EXPECT_LT(up.card_bottom + 8.f, 800.f - keyboard) << "the card is clear of the keyboard";
  KeyboardDown();
  const Snap down = Snapshot();
  EXPECT_EQ(down.lift, rest_.lift);
  EXPECT_FLOAT_EQ(down.card_top, rest_.card_top);
  EXPECT_FLOAT_EQ(down.logo_top, rest_.logo_top);
  EXPECT_FLOAT_EQ(down.chips_top, rest_.chips_top);
}

TEST_F(HomeKeyboardDockTest, AShortScreenLiftsTheBlockByExactlyTheOverlap) {
  const float overlap = 40.f;
  const int keyboard = static_cast<int>(800.f - rest_.card_bottom - 8.f + overlap);
  KeyboardUp(keyboard);
  const Snap up = Snapshot();
  std::printf("[ShellHost short] keyboard %d (top %d) lift %.1f card bottom %.1f (rest %.1f) logo %.1f (rest %.1f)\n",
              keyboard, 800 - keyboard, up.lift, up.card_bottom, rest_.card_bottom, up.logo_top, rest_.logo_top);
  EXPECT_EQ(up.lift, rest_.lift) << "the root is not lifted: the block is";
  EXPECT_NEAR(up.card_bottom, 800.f - keyboard - 8.f, 1.f);
  EXPECT_NEAR(rest_.logo_top - up.logo_top, overlap, 1.f) << "the mascot moved by the same overlap, not more";
  KeyboardDown();
  const Snap down = Snapshot();
  EXPECT_FLOAT_EQ(down.card_top, rest_.card_top);
  EXPECT_FLOAT_EQ(down.logo_top, rest_.logo_top);
}

// The sheet (280dp by default, taller than the room under this window's card) follows the keyboard's rule with its own
// height: the root is not lifted, the block moves up just enough to keep the card 8dp above the sheet, and it returns.
TEST_F(HomeKeyboardDockTest, TheEmojiSheetFollowsTheSameRule) {
  BottomChromeSpec spec;
  spec.key = "emoji_panel";
  host_.SetBottomChrome(spec);
  Settle();
  const Snap up = Snapshot();
  const float sheet = 280.f;  // ShellHost kDefaultEmojiKeyboardPanelDp (no keyboard was up before)
  std::printf("[ShellHost sheet] sheet %.0f (top %.0f) lift %.1f card bottom %.1f (rest %.1f) logo %.1f (rest %.1f)\n", sheet,
              800.f - sheet, up.lift, up.card_bottom, rest_.card_bottom, up.logo_top, rest_.logo_top);
  EXPECT_EQ(up.lift, rest_.lift) << "the root is not lifted";
  EXPECT_NEAR(up.card_bottom, std::min(rest_.card_bottom, 800.f - sheet - 8.f), 1.f);
  EXPECT_NEAR(rest_.logo_top - up.logo_top, std::max(0.f, rest_.card_bottom + 8.f - (800.f - sheet)), 1.f);
  host_.ClearBottomChrome();
  Settle();
  const Snap down = Snapshot();
  EXPECT_FLOAT_EQ(down.card_top, rest_.card_top);
  EXPECT_FLOAT_EQ(down.logo_top, rest_.logo_top);
  EXPECT_EQ(down.lift, rest_.lift);
}

TEST_F(HomeKeyboardDockTest, AChatPageKeepsTheFullLift) {
  host_.SetPrimaryPane("chat");
  host_.OpenCompactChat();
  Settle();
  const float rest = root_->GetProperty<float>("bottom");
  KeyboardUp(300);
  EXPECT_GE(root_->GetProperty<float>("bottom"), rest + 300.f - 0.5f) << "full lift on any other page";
  KeyboardDown();
  EXPECT_EQ(root_->GetProperty<float>("bottom"), rest);
}

TEST_F(ShellDrawerTest, TheLayoutBecomingTheDesktopsClosesIt) {
  host_.OpenAiDrawer();
  Settle();
  context_->SetDimensions({1200, 800});
  host_.Update(context_);
  EXPECT_EQ(host_.State().layout_mode, LayoutMode::Expanded);
  EXPECT_FALSE(host_.State().ai_drawer_open);
  Settle();
  EXPECT_EQ(ById("ai-drawer-sessions"), nullptr);
}

} // namespace
} // namespace pbr
