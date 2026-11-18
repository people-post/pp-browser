// The phone Chats bar's scan button (left of compose): the real sidebar.rml bar and title row with their bindings, the
// real stylesheets and fonts, the window model of a real ShellHost. A tap on scan runs the shell's scan action (the app
// binds it to ContactsController::OnScanQr, the same scan as Add contact's) once; at 320dp, in English and Chinese,
// neither the bar's title nor the title row's status runs under the two buttons.
#include "foundation/i18n/LocalizationService.h"
#include "gui/shell/DataModelHost.h"
#include "gui/shell/ShellHost.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/ElementUtilities.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace pbr {
namespace {

class NullRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i& dimensions, const ui::String&) override {
    dimensions = {24, 24};
    return 2;
  }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

std::string AssetPath(const std::string& relative) {
  return (std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative).string();
}

std::string Asset(const std::string& relative) {
  std::ifstream in(AssetPath(relative), std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

/** {{i18n:key}} filled with the current language's words, as the app's text view does. */
std::string Translated(std::string rml) {
  const std::regex key(R"(\{\{i18n:([^}]*)\}\})");
  std::smatch match;
  std::string out;
  while (std::regex_search(rml, match, key)) {
    out += match.prefix().str() + Tr(match[1].str());
    rml = match.suffix().str();
  }
  return out + rml;
}

constexpr const char* kContext = "chats-scan-button-test";

class ChatsScanButtonTest : public testing::Test {
protected:
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext(kContext);
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
    LocalizationService::Instance().SetPreferredLanguage("en");
  }

  void Load(const int width, const char* language) {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage(language);
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    // The app's faces (Application.cpp); Chinese falls back to Noto Sans SC (bold: the widest it can be).
    ASSERT_TRUE(ui::LoadFontFace(AssetPath("fonts/LatoLatin-Regular.ttf")));
    for (const char* file :
         {"fonts/Inter-Regular.ttf", "fonts/Inter-Medium.ttf", "fonts/Inter-SemiBold.ttf", "fonts/Inter-Bold.ttf"}) {
      ASSERT_TRUE(ui::LoadFontFace(AssetPath(file)));
    }
    ASSERT_TRUE(ui::LoadFontFace(AssetPath("fonts/NotoSansSC-Bold.otf"), /*fallback_face=*/true));
    context = ui::CreateContext(kContext, {width, 844});
    ASSERT_NE(context, nullptr);
    context->ActivateTheme("light", true);

    host.BindScanAction([this]() { ++scans; });
    ASSERT_TRUE(host.RegisterWindowModel(context));
    host.SetChatsLink(ChatsLink::Connecting); // the longest status word
    // The Chats panel's own model (ChatController's "shell"): only the compose event is needed here.
    ASSERT_TRUE(DataModelHost::Instance().Register(context, "shell", [this](ui::DataModelConstructor& ctor) {
      ctor.BindEventCallback("open_new_session_menu",
                             [this](ui::DataModelHandle, ui::Event&, const ui::VariantList&) { ++composes; });
    }));

    const std::string sidebar = Asset("views/sidebar.rml");
    const size_t bar_begin = sidebar.find("<div class=\"pane-nav\">");
    const size_t bar_end = sidebar.find("<div class=\"sidebar-header row\">");
    const size_t title_begin = sidebar.find("<div class=\"large-title\" data-model=\"window\">");
    const size_t title_end = sidebar.find("<div class=\"chat-search\">");
    ASSERT_NE(bar_begin, std::string::npos);
    ASSERT_NE(title_begin, std::string::npos);
    const std::string rml =
        "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
        Asset("themes/colors-light.rcss") +
        "</style></head><body style=\"padding: 0;\">"
        "<div class=\"shell-layer-compact\" style=\"position: absolute; top: 0dp; left: 0dp; "
        "width: " +
        std::to_string(width) + "dp; height: 800dp;\"><div class=\"sidebar-panel\" data-model=\"shell\">" +
        Translated(sidebar.substr(bar_begin, bar_end - bar_begin)) + "<div class=\"sidebar-sessions root-scroll\">" +
        Translated(sidebar.substr(title_begin, title_end - title_begin)) + "</div></div></div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    context->Update();
  }

  ui::Element* One(const char* cls) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    return found.empty() ? nullptr : found.front();
  }
  ui::Element* Compose() {
    ui::ElementList found;
    document->GetElementsByClassName(found, "pane-nav-btn");
    for (ui::Element* button : found) {
      if (!button->IsClassSet("pane-nav-btn--scan")) {
        return button;
      }
    }
    return nullptr;
  }
  static float Left(ui::Element* e) { return e->GetAbsoluteOffset(ui::BoxArea::Border).x; }
  static float Right(ui::Element* e) { return Left(e) + e->GetBox().GetSize(ui::BoxArea::Border).x; }
  static ui::Vector2f SizeOf(ui::Element* e) { return e->GetBox().GetSize(ui::BoxArea::Border); }

  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  ShellHost host;
  int scans = 0;
  int composes = 0;
};

TEST_F(ChatsScanButtonTest, ScanSitsLeftOfComposeAndATapRunsTheScanOnce) {
  Load(390, "en");
  ui::Element* scan = One("pane-nav-btn--scan");
  ui::Element* compose = Compose();
  ASSERT_NE(scan, nullptr);
  ASSERT_NE(compose, nullptr);
  EXPECT_EQ(scan->GetAttribute<ui::String>("data-event-click", ""), "open_scan_qr()");
  EXPECT_FLOAT_EQ(SizeOf(scan).x, 44.f);
  EXPECT_FLOAT_EQ(SizeOf(scan).y, 44.f);
  EXPECT_FLOAT_EQ(Right(compose), 390.f - 12.f) << "compose keeps the top right";
  EXPECT_FLOAT_EQ(Left(compose) - Right(scan), 4.f) << "scan, then compose, 4dp apart";
  EXPECT_FLOAT_EQ(scan->GetAbsoluteOffset(ui::BoxArea::Border).y, compose->GetAbsoluteOffset(ui::BoxArea::Border).y);
  ui::Element* glyph = scan->GetFirstChild();
  ASSERT_NE(glyph, nullptr);
  EXPECT_EQ(glyph->GetAttribute<ui::String>("src", ""), "../icons/v2/scan.svg");
  EXPECT_FLOAT_EQ(SizeOf(glyph).x, 24.f);
  EXPECT_EQ(scan->GetComputedValues().pointer_events(), ui::Style::PointerEvents::Auto)
      << "the bar lets touches through, but not on its buttons";

  scan->DispatchEvent(ui::EventId::Click, {});
  context->Update();
  EXPECT_EQ(scans, 1);
  EXPECT_EQ(composes, 0);
  compose->DispatchEvent(ui::EventId::Click, {});
  context->Update();
  EXPECT_EQ(scans, 1);
  EXPECT_EQ(composes, 1);
}

TEST_F(ChatsScanButtonTest, AtThreeHundredTwentyNothingRunsUnderTheButtons) {
  for (const char* language : {"en", "zh-Hans"}) {
    SCOPED_TRACE(language);
    if (context) {
      DataModelHost::Instance().Clear();
      ui::RemoveContext(kContext);
      context = nullptr;
      ui::Shutdown();
      initialized = false;
    }
    Load(320, language);
    ui::Element* scan = One("pane-nav-btn--scan");
    ui::Element* title = One("pane-nav-title");
    ui::Element* status = One("link-status");
    ui::Element* word = One("large-title-text");
    ASSERT_NE(scan, nullptr);
    ASSERT_NE(title, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(word, nullptr);
    // The bar's small title (shown once the list has scrolled) is centred clear of the buttons and fits whole.
    EXPECT_LE(Right(title), Left(scan) + 0.5f);
    EXPECT_FLOAT_EQ(Left(title), 116.f) << "the same 116dp margin on the left: centred";
    const int title_width = ui::ElementUtilities::GetStringWidth(title, Tr("nav.sessions"));
    EXPECT_GT(title_width, 0) << "fonts loaded";
    EXPECT_LE(static_cast<float>(title_width), SizeOf(title).x) << "not clipped";
    // The title row: "Chats" / "聊天", then the status word, all left of the scan button.
    EXPECT_GT(SizeOf(word).x, 0.f);
    EXPECT_GT(SizeOf(status).x, 0.f);
    EXPECT_GE(Left(status), Right(word) - 0.5f);
    EXPECT_LE(Right(status), Left(scan) + 0.5f) << "the status stays clear of the scan button";
  }
}

} // namespace
} // namespace pbr
