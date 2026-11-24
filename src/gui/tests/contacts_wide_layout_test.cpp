// The Contacts page in the wide layout (design step 3, page A): contacts_wide.rml (the list column) and
// contact_detail_wide.rml (the selected contact) over the controller's real data model and the real stylesheets, at the
// widths the window gives them at 768 / 900 / 1280. The rows are the phone's (44dp avatar, bold name, star), the page is the
// phone's contact page in a centred column, and nothing is clipped.
#include "common/runtime/AppRuntime.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/contacts/ContactsController.h"
#include "gui/shell/DataModelHost.h"
#include "lib/ui/ViewCatalog.h"

#include <ui/base/SystemInterface.h>
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
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

class QuietSystem : public ui::SystemInterface {
public:
  double GetElapsedTime() override { return 0.0; }
};

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

std::string Asset(const char* relative) {
  return ReadAll(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
}

/** A view as the shell mounts it: includes and words expanded (ViewCatalog::LoadBody). */
std::string View(const char* key) {
  return ViewCatalog::LoadBody(ViewCatalog::KeyForLayout(key, true));
}

constexpr const char* kContext = "contacts-wide-layout-test";
constexpr int kRail = 64;
constexpr int kList = 320;

class ContactsWideLayoutTest : public ::testing::Test {
protected:
  void SetUp() override {
    ContactsController::InstallInstance(controller_);
    AppRuntime::Initialize(AppRuntimeConfig{});
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    ui::SetRenderInterface(&render_);
    ui::SetSystemInterface(&system_);
    ASSERT_TRUE(ui::Initialise());
    ASSERT_TRUE(ui::LoadFontFace(std::string(PP_BROWSER_ASSETS_DIR) + "/fonts/LatoLatin-Regular.ttf"));
    ASSERT_TRUE(ui::LoadFontFace(std::string(PP_BROWSER_ASSETS_DIR) + "/fonts/Inter-Regular.ttf", true));
    context_ = ui::CreateContext(kContext, {1280, 800});
    ASSERT_NE(context_, nullptr);

    ShellNavigationPorts nav;
    nav.snapshot = []() {
      ShellChromeSnapshot chrome;
      chrome.layout_mode = LayoutMode::Expanded;
      chrome.nav_tab = NavTab::Contacts;
      return chrome;
    };
    nav.set_primary_pane = [this](const std::string& key) { primary_ = key; };
    nav.clear_primary_pane = [this]() { primary_.clear(); };
    controller_.BindShellNavigation(nav);

    MessagingContactsPorts ports;
    ports.snapshot = []() {
      MessagingView view;
      view.initialized = true;
      return view;
    };
    store_ = {MakeContact("c-ann", "Ann Lee"),
              MakeContact("c-alexander", "Alexander Maximilian von Hohenzollern-Sigmaringen III")};
    ports.list_contacts = [this]() -> Roe<std::vector<Contact>> { return store_; };
    ports.get_contact = [this](const std::string& id) -> Roe<std::optional<Contact>> {
      for (const Contact& contact : store_) {
        if (contact.id == id) {
          return std::optional<Contact>{contact};
        }
      }
      return std::optional<Contact>{};
    };
    controller_.BindContactsPorts(ports);
    controller_.OnNavTabActivated();
    ASSERT_TRUE(controller_.RegisterModel(context_));
  }
  void TearDown() override {
    controller_.BindContactsPorts({});
    controller_.BindShellNavigation({});
    DataModelHost::Instance().Clear();
    ui::RemoveContext(kContext);
    ui::Shutdown();
    ui::SetSystemInterface(nullptr);
    ui::SetRenderInterface(nullptr);
    AppRuntime::Shutdown();
    ContactsController::ClearInstance();
  }

  static Contact MakeContact(const std::string& id, const std::string& name) {
    Contact contact;
    contact.id = id;
    contact.display_name = name;
    contact.remote.nickname = name;
    contact.remote.ids = {{mesh::ContactIdKind::Account, "account:" + id, true}};
    SyncContactMirrors(contact);
    return contact;
  }

  /** The list column and the detail column side by side in a wide-root window `window_dp` wide. */
  void Load(const int window_dp) {
    const int list = std::max(280, std::min(kList, window_dp - kRail - 420));
    const int detail = window_dp - kRail - list;
    const std::string rml =
        "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
        Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") +
        "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
        "<div class=\"shell-body shell-body--wide\" style=\"position: absolute; top: 0; left: 0; width: " +
        std::to_string(window_dp) +
        "dp; height: 700dp;\"><div class=\"shell-layer shell-layer-base\">"
        "<div id=\"list\" class=\"shell-pane shell-pane-secondary\" style=\"position: absolute; left: " +
        std::to_string(kRail) + "dp; width: " + std::to_string(list) +
        "dp; height: 700dp;\"><div class=\"shell-pane-body\">" + View("contacts") +
        "</div></div><div id=\"detail\" class=\"shell-pane shell-pane-primary\" style=\"position: absolute; left: " +
        std::to_string(kRail + list) + "dp; width: " + std::to_string(detail) +
        "dp; height: 700dp;\"><div class=\"shell-pane-body\">" + View("contact_detail") +
        "</div></div></div></div></body></rml>";
    document_ = context_->LoadDocumentFromMemory(rml);
    ASSERT_NE(document_, nullptr);
    document_->Show();
    context_->Update();
    context_->Update();
  }
  struct Box {
    float left, top, right, bottom;
    float W() const { return right - left; }
    float H() const { return bottom - top; }
  };
  static Box BoxOf(ui::Element* element) {
    const ui::Vector2f at = element->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f size = element->GetBox().GetSize(ui::BoxArea::Border);
    return {at.x, at.y, at.x + size.x, at.y + size.y};
  }
  /** The elements drawn: data-for leaves a hidden template after its rows and data-if only hides, so zero-size ones
   *  (display: none) are left out. */
  ui::ElementList All(const char* selector) {
    ui::ElementList found;
    document_->QuerySelectorAll(found, selector);
    ui::ElementList drawn;
    for (ui::Element* element : found) {
      if (BoxOf(element).H() > 0.f) {
        drawn.push_back(element);
      }
    }
    return drawn;
  }
  ui::Element* One(const char* selector) {
    const ui::ElementList found = All(selector);
    return found.empty() ? nullptr : found[0];
  }
  void Pick(const size_t row) {
    const ui::ElementList rows = All(".contacts-row");
    ASSERT_LT(row, rows.size());
    rows[row]->DispatchEvent(ui::EventId::Click, {});
    context_->Update();
    context_->Update();
  }

  NullRender render_;
  QuietSystem system_;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ContactsController controller_;
  std::vector<Contact> store_;
  std::string primary_;
};

TEST_F(ContactsWideLayoutTest, TheListIsThePhonesRowsInsideTheColumnAtEveryWidth) {
  for (const int window : {768, 900, 1280}) {
    SCOPED_TRACE(window);
    Load(window);
    const Box column = BoxOf(One("#list"));
    ui::ElementList rows = All(".contacts-row");
    ASSERT_EQ(rows.size(), 2u);
    for (ui::Element* row : rows) {
      const Box box = BoxOf(row);
      EXPECT_GE(box.H(), 44.f) << "a click target of at least 44dp";
      EXPECT_GE(box.left, column.left);
      EXPECT_LE(box.right, column.right) << "no row spills out of the column";
    }
    // The phone's row: a 44dp avatar, then the name; no badge, no account line.
    ui::Element* avatar = One(".contacts-row-avatar-frame");
    ASSERT_NE(avatar, nullptr);
    EXPECT_FLOAT_EQ(BoxOf(avatar).W(), 44.f);
    EXPECT_FLOAT_EQ(BoxOf(avatar).H(), 44.f);
    EXPECT_EQ(All(".contacts-row-subtitle--desktop").size(), 0u) << "the phone row shows the name only";
    EXPECT_EQ(All(".contacts-trust-badge").size(), 0u);
    // The plus is at the header's right end, the search field under the title.
    ui::Element* plus = One(".sidebar-new-chat");
    ASSERT_NE(plus, nullptr);
    EXPECT_LE(BoxOf(plus).right, column.right);
    EXPECT_GT(BoxOf(One(".chat-search")).top, BoxOf(One(".sidebar-header")).top);
  }
}

TEST_F(ContactsWideLayoutTest, TheSelectedContactIsThePhonesPageInACentredColumn) {
  for (const int window : {768, 900, 1280}) {
    SCOPED_TRACE(window);
    Load(window);
    Pick(0);
    EXPECT_EQ(primary_, "contact_detail");
    const Box pane = BoxOf(One("#detail"));
    ui::Element* panel = One(".contact-detail-panel");
    ASSERT_NE(panel, nullptr);
    EXPECT_LE(BoxOf(panel).W(), 640.f) << "a column, not the whole window";
    EXPECT_GE(BoxOf(panel).left, pane.left);
    EXPECT_LE(BoxOf(panel).right, pane.right);
    // The phone's page: a 96dp avatar, cards, and the Block / Remove card; the older desktop blocks are gone.
    EXPECT_FLOAT_EQ(BoxOf(One(".contact-detail-avatar-frame")).W(), 96.f);
    EXPECT_GE(All(".cd-card").size(), 3u);
    EXPECT_EQ(All(".contacts-detail-body").size(), 0u);
    EXPECT_EQ(All(".contacts-actions").size(), 0u);
    for (ui::Element* card : All(".cd-card")) {
      EXPECT_LE(BoxOf(card).right, BoxOf(panel).right);
    }
    for (ui::Element* row : All(".cd-row")) {
      EXPECT_GE(BoxOf(row).H(), 44.f);
    }
  }
}

// One page body, two files: the wide page's cards are the phone page's `cd-phone` block, character for character.
TEST_F(ContactsWideLayoutTest, TheWideContactPageHasTheSameCardsAsThePhonePage) {
  const std::string phone = Asset("views/contact_detail.rml");
  const std::string wide = Asset("views/contact_detail_wide.rml");
  const std::string open = "<div class=\"cd-phone\">\n";
  const size_t phone_at = phone.find(open);
  const size_t wide_at = wide.find(open);
  ASSERT_NE(phone_at, std::string::npos);
  ASSERT_NE(wide_at, std::string::npos);
  const std::string phone_cards = phone.substr(
      phone_at + open.size(), phone.find("\n  <div class=\"contacts-detail-body stack\">") - phone_at - open.size());
  EXPECT_NE(wide.find(phone_cards.substr(0, phone_cards.rfind("  </div>"))), std::string::npos)
      << "contact_detail_wide.rml's cards drifted from contact_detail.rml's cd-phone block";
}

// What a left swipe does on a phone contact row (Info, Voice, Video, More) is reachable here without a swipe: the row's
// hover buttons (hidden until the pointer is over the row, 44dp when shown) and a right-click menu with the same four.
TEST_F(ContactsWideLayoutTest, TheSwipeActionsAreHoverButtonsAndARightClickMenu) {
  Load(768);
  EXPECT_EQ(All(".contacts-row-action").size(), 0u) << "not drawn until the pointer is over a row";
  ui::ElementList all_rows;
  document_->QuerySelectorAll(all_rows, ".contacts-row-actions");
  ASSERT_GE(all_rows.size(), 2u);
  all_rows[0]->SetProperty("display", "flex"); // what :hover does
  context_->Update();
  context_->Update();
  const ui::ElementList buttons = All(".contacts-row-action");
  ASSERT_EQ(buttons.size(), 3u) << "Voice, Video, More";
  const Box column = BoxOf(One("#list"));
  for (ui::Element* button : buttons) {
    EXPECT_GE(BoxOf(button).W(), 44.f);
    EXPECT_GE(BoxOf(button).H(), 44.f);
    EXPECT_LE(BoxOf(button).right, column.right);
  }
  // The row still shows its name beside them.
  EXPECT_GT(BoxOf(One(".contacts-row-title")).W(), 0.f);

  std::vector<std::string> ids;
  for (const ContextMenuAction& action : controller_.ContactRowMenuActions("c-ann")) {
    ids.push_back(action.id);
  }
  EXPECT_EQ(ids, (std::vector<std::string>{"contact_info", "contact_voice", "contact_video", "block", "remove"}));
  EXPECT_TRUE(controller_.ContactRowMenuActions("").empty());
}

TEST_F(ContactsWideLayoutTest, ALongNameDoesNotPushTheRowOrThePageOutOfTheirColumns) {
  Load(768);
  Pick(0); // sorted: "Alexander Maximilian von Hohenzollern-Sigmaringen III" first
  const Box column = BoxOf(One("#list"));
  for (ui::Element* title : All(".contacts-row-title")) {
    EXPECT_LE(BoxOf(title).right, column.right);
  }
  EXPECT_LE(BoxOf(One(".contact-detail-title-block")).right, BoxOf(One("#detail")).right);
}

} // namespace
} // namespace pbr
