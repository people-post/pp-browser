// The Settings page in the wide layout (design step 3, page B): settings_wide.rml (the list column) with the real
// stylesheets, at the widths the window gives it at 768 / 900 / 1280, over a small stand-in for the settings data model
// (the page binds only a handful of fields); and the page's names, order and groups checked against the phone's page.
#include "foundation/i18n/LocalizationService.h"
#include "gui/shell/DataModelHost.h"
#include "lib/ui/ViewCatalog.h"

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

std::string Asset(const char* relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

struct SectionRow {
  ui::String id;
  ui::String title;
  ui::String subtitle;
  int group = 0;
  bool attention = false;
};

constexpr const char* kContext = "settings-wide-layout-test";
constexpr int kRail = 64;
constexpr int kList = 320;

class SettingsWideLayoutTest : public testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    ui::SetRenderInterface(&render_);
    ASSERT_TRUE(ui::Initialise());
    ASSERT_TRUE(ui::LoadFontFace(std::string(PP_BROWSER_ASSETS_DIR) + "/fonts/LatoLatin-Regular.ttf"));
    ASSERT_TRUE(ui::LoadFontFace(std::string(PP_BROWSER_ASSETS_DIR) + "/fonts/Inter-Regular.ttf", true));
    context_ = ui::CreateContext(kContext, {1280, 800});
    ASSERT_NE(context_, nullptr);
    sections_ = {{"privacy", "Privacy", "", 1},
                 {"security", "Security", "", 1},
                 {"devices", "Linked devices", "", 1},
                 {"notifications", "Notifications", "", 2},
                 {"appearance", "Appearance", "", 2},
                 {"storage", "Storage", "", 2},
                 {"help", "Help", "", 3}};
    ASSERT_TRUE(DataModelHost::Instance().Register(context_, "settings", [this](ui::DataModelConstructor& ctor) {
      if (auto handle = ctor.RegisterStruct<SectionRow>()) {
        handle.RegisterMember("id", &SectionRow::id);
        handle.RegisterMember("title", &SectionRow::title);
        handle.RegisterMember("subtitle", &SectionRow::subtitle);
        handle.RegisterMember("group", &SectionRow::group);
        handle.RegisterMember("attention", &SectionRow::attention);
      }
      ctor.RegisterArray<std::vector<SectionRow>>();
      ctor.Bind("sections", &sections_);
      ctor.Bind("selected_id", &selected_id_);
      ctor.Bind("status", &status_);
      ctor.Bind("profile_has_icon", &no_icon_);
      ctor.Bind("profile_icon_src", &icon_src_);
      ctor.Bind("profile_avatar_letter", &letter_);
      ctor.Bind("profile_avatar_tone", &tone_);
      ctor.Bind("profile_nickname", &nickname_);
      ctor.Bind("selected_title", &selected_title_);
      for (const char* name : {"select_section", "copy_profile_id", "share_profile", "show_my_qr"}) {
        ctor.BindEventCallback(name, [this, name](ui::DataModelHandle, ui::Event&, const ui::VariantList& args) {
          calls_.push_back(std::string(name) +
                           (args.empty() ? "" : ":" + std::string(args[0].Get<ui::String>().c_str())));
        });
      }
    }));
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    ui::RemoveContext(kContext);
    ui::Shutdown();
    ui::SetRenderInterface(nullptr);
  }

  void Load(const int window_dp) {
    const int list = std::max(280, std::min(kList, window_dp - kRail - 420));
    const std::string page = LocalizationService::Instance().LocalizeText(Asset("views/settings_wide.rml"));
    const std::string rml =
        "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
        Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") +
        "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
        "<div class=\"shell-body shell-body--wide\" style=\"position: absolute; top: 0; left: 0; width: " +
        std::to_string(window_dp) +
        "dp; height: 700dp;\"><div class=\"shell-layer shell-layer-base\">"
        "<div id=\"list\" class=\"shell-pane shell-pane-secondary\" style=\"position: absolute; left: " +
        std::to_string(kRail) + "dp; width: " + std::to_string(list) +
        "dp; height: 700dp;\"><div class=\"shell-pane-body\">" + page + "</div></div></div></div></body></rml>";
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
  ui::ElementList All(const char* selector) {
    ui::ElementList found;
    document_->QuerySelectorAll(found, selector);
    ui::ElementList drawn; // data-for leaves a hidden template element after its rows
    for (ui::Element* element : found) {
      if (BoxOf(element).H() > 0.f) {
        drawn.push_back(element);
      }
    }
    return drawn;
  }

  NullRender render_;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  std::vector<SectionRow> sections_;
  ui::String selected_id_;
  ui::String status_;
  bool no_icon_ = false;
  ui::String icon_src_;
  ui::String letter_ = "U";
  int tone_ = 3;
  ui::String nickname_ = "user";
  ui::String selected_title_;
  std::vector<std::string> calls_;
};

TEST_F(SettingsWideLayoutTest, TheListIsThePhonesCardAndThreeGroupsInOrderInsideTheColumnAtEveryWidth) {
  for (const int window : {768, 900, 1280}) {
    SCOPED_TRACE(window);
    Load(window);
    const Box column = BoxOf(All("#list")[0]);
    const ui::ElementList cards = All(".group-card");
    ASSERT_EQ(cards.size(), 4u) << "the card, then the three groups";
    EXPECT_TRUE(cards[0]->IsClassSet("me-card"));
    for (size_t i = 1; i < cards.size(); ++i) {
      EXPECT_GT(BoxOf(cards[i]).top, BoxOf(cards[i - 1]).bottom) << "in order, apart";
      EXPECT_GE(BoxOf(cards[i]).left, column.left);
      EXPECT_LE(BoxOf(cards[i]).right, column.right);
    }
    const ui::ElementList rows = All(".group-row");
    ASSERT_EQ(rows.size(), 7u);
    std::vector<std::string> names;
    for (ui::Element* row : rows) {
      EXPECT_GE(BoxOf(row).H(), 44.f) << "a click target of at least 44dp";
      EXPECT_LE(BoxOf(row).right, column.right);
      ui::ElementList text;
      row->GetElementsByClassName(text, "group-row-text");
      ASSERT_FALSE(text.empty());
      names.emplace_back(text[0]->GetInnerRML().c_str());
    }
    EXPECT_EQ(names, (std::vector<std::string>{"Privacy", "Security", "Linked devices", "Notifications", "Appearance",
                                               "Storage", "Help"}));
    // Rows 0-2 sit in the second card, 3-5 in the third, 6 in the fourth: the phone's grouping.
    EXPECT_LT(BoxOf(rows[2]).bottom, BoxOf(cards[2]).top + 1.f);
    EXPECT_GE(BoxOf(rows[3]).top, BoxOf(cards[2]).top - 1.f);
    EXPECT_GE(BoxOf(rows[6]).top, BoxOf(cards[3]).top - 1.f);
    // The card: avatar and name over the three labelled buttons, all inside it.
    const Box card = BoxOf(cards[0]);
    ui::ElementList acts = All(".me-card-act");
    ASSERT_EQ(acts.size(), 3u) << "Copy, Share, QR code";
    for (ui::Element* act : acts) {
      EXPECT_GE(BoxOf(act).left, card.left);
      EXPECT_LE(BoxOf(act).right, card.right);
      EXPECT_GE(BoxOf(act).H(), 44.f);
      EXPECT_LE(BoxOf(act).bottom, card.bottom);
    }
    ui::Element* avatar = All(".me-card-avatar")[0];
    EXPECT_LE(BoxOf(avatar).bottom, BoxOf(acts[0]).top + 1.f) << "the buttons are under the name, not over the avatar";
    EXPECT_GT(BoxOf(All(".me-card-name")[0]).W(), 20.f) << "the name has room";
  }
}

TEST_F(SettingsWideLayoutTest, ARowOpensItsPageAndThePickedRowIsMarked) {
  Load(900);
  ui::ElementList rows = All(".group-row");
  ASSERT_EQ(rows.size(), 7u);
  rows[4]->DispatchEvent(ui::EventId::Click, {});
  ASSERT_EQ(calls_.size(), 1u);
  EXPECT_EQ(calls_[0], "select_section:appearance");
  for (ui::Element* act : All(".me-card-act")) {
    act->DispatchEvent(ui::EventId::Click, {});
  }
  EXPECT_EQ(calls_,
            (std::vector<std::string>{"select_section:appearance", "copy_profile_id", "share_profile", "show_my_qr"}));
  EXPECT_EQ(All(".group-row--active").size(), 0u);
  document_->Close();
  context_->Update();
  selected_id_ = "storage";
  DataModelHost::Instance().Dirty("settings", "selected_id");
  Load(900);
  const ui::ElementList active = All(".group-row--active");
  ASSERT_EQ(active.size(), 1u);
  ui::ElementList text;
  active[0]->GetElementsByClassName(text, "group-row-text");
  EXPECT_EQ(std::string(text[0]->GetInnerRML().c_str()), "Storage");
}

// The page's names, order and groups are the phone's: the same sections through the same groups, the same card.
TEST_F(SettingsWideLayoutTest, TheWideListHasTheSameCardRowsAndEventsAsThePhonePage) {
  const std::string phone = Asset("views/settings.rml");
  const std::string wide = Asset("views/settings_wide.rml");
  auto events = [](const std::string& text) {
    std::vector<std::string> found;
    for (size_t at = text.find("data-event-click=\""); at != std::string::npos;
         at = text.find("data-event-click=\"", at + 1)) {
      const size_t from = at + 18;
      found.push_back(text.substr(from, text.find('"', from) - from));
    }
    return found;
  };
  // The phone block is the one marked settings-phone-only; the desktop block after it is the older list.
  const size_t begin = phone.find("<div class=\"settings-phone-only me-home\">");
  const size_t end = phone.find("class=\"me-profile-card me-identity-header");
  ASSERT_NE(begin, std::string::npos);
  ASSERT_NE(end, std::string::npos);
  const std::vector<std::string> phone_events = events(phone.substr(begin, end - begin));
  const std::vector<std::string> wide_events = events(wide);
  EXPECT_EQ(phone_events, wide_events) << "the same handlers in the same order";
  for (const char* group : {"section.group == 1", "section.group == 2", "section.group == 3"}) {
    EXPECT_NE(phone.find(group), std::string::npos);
    EXPECT_NE(wide.find(group), std::string::npos);
  }
  EXPECT_LT(wide.find("section.group == 1"), wide.find("section.group == 2"));
  EXPECT_LT(wide.find("section.group == 2"), wide.find("section.group == 3"));
  for (const char* word : {"settings.card.caption", "settings.card.copy", "settings.card.share", "settings.card.qr"}) {
    EXPECT_NE(wide.find(word), std::string::npos) << word;
  }
  EXPECT_EQ(wide.find("settings.preferences"), std::string::npos) << "the old \"Preferences\" list is not used";
  EXPECT_EQ(wide.find("settings-phone-only"), std::string::npos);
}

// Where a phone page needs its pushed-page stack (a PIN row that opens a page, the transfer page, the pickers' sheets)
// the wide page keeps the desktop's inline version; the pieces marked settings-wide-too have nothing page-like in them,
// and My profile shows the phone's rows (the page's wrapper carries settings-detail--profile).
TEST_F(SettingsWideLayoutTest, PhoneOnlyPiecesAreHiddenInTheWideLayoutUnlessMarkedWideTooOrOnMyProfile) {
  const std::string rml =
      "<rml><head><style>" + Asset("themes/components.rcss") + Asset("themes/wide.rcss") +
      "</style></head><body><div class=\"shell-body shell-body--wide\"><div class=\"settings-section--grouped\">"
      "<div id=\"pin\" class=\"group-row settings-phone-only\">PIN</div>"
      "<div id=\"dot\" class=\"group-card settings-phone-only settings-wide-too\">Dot</div>"
      "<div class=\"settings-detail-panel settings-detail--profile\">"
      "<div id=\"nick\" class=\"group-card settings-phone-only\">Nickname</div>"
      "<p id=\"head\" class=\"group-head settings-phone-only\">Account</p>"
      "<div id=\"field\" class=\"group-card settings-desktop-only\">field</div>"
      "<button id=\"copy\" class=\"btn settings-desktop-only\">Copy</button></div>"
      "</div></div></body></rml>";
  ui::ElementDocument* doc = context_->LoadDocumentFromMemory(rml);
  ASSERT_NE(doc, nullptr);
  doc->Show();
  context_->Update();
  auto none = [&](const char* id) {
    return doc->GetElementById(id)->GetComputedValues().display() == ui::Style::Display::None;
  };
  EXPECT_TRUE(none("pin")) << "a phone-only row that opens a page stays out";
  EXPECT_FALSE(none("dot")) << "marked wide-too";
  EXPECT_FALSE(none("nick")) << "My profile: the phone's rows";
  EXPECT_TRUE(none("head")) << "the phone has no headings in a grouped page";
  EXPECT_TRUE(none("field")) << "My profile: the desktop's field and buttons give way";
  EXPECT_TRUE(none("copy"));
  doc->Close();
}

} // namespace
} // namespace pbr
