// The recovery phrase page (Kenneth 2026-11-19): 24 numbered cards, three to a row, opened covered (no word in the
// document) and revealed by one tap. The real settings.rml page block and the real security actions over a real data
// model on the real stylesheets; the PIN gate is stubbed to say yes at once.
#include "gui/SettingsHost.h"
#include "gui/SettingsSecurityActions.h"
#include "gui/UserFeedback.h"
#include "gui/shell/DataModelHost.h"

#include "common/runtime/AppRuntime.h"
#include "foundation/data/Config.h"
#include "foundation/data/SessionStore.h"
#include "foundation/data/UserPreferences.h"
#include "foundation/i18n/LocalizationService.h"

#include <ui/core/Core.h>
#include <ui/data/DataModelHandle.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
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
  ui::TextureHandle LoadTexture(ui::Vector2i& size, const ui::String&) override {
    size = {20, 20};
    return 2;
  }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

std::string ReadAll(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

/** Wide 7 and 8 letter words: the page must hold the widest a phrase can bring. */
const std::vector<std::string> kWords = {"mushroom", "marriage", "midnight", "swimming", "wrestle", "willow",
                                         "women",    "window",   "wonder",   "mixture",  "museum",  "minimum",
                                         "mammal",   "million",  "mimic",    "mom",      "magnet",  "maximum",
                                         "mixed",    "mom",      "wisdom",   "weekend",  "welcome", "windows"};

class FakeHost final : public SettingsHost {
public:
  explicit FakeHost(SessionStore& store) : store_(store) {}
  SessionStore& Store() override { return store_; }
  void ReportFailure(const Error&) override {}
  void ShowStatus(const std::string&) override {}
  void Resync(bool) override {}
  SettingsUiState& UiState() override { return state_; }
  void PullFromView() override {}
  void PushToView(bool) override {}
  void SaveProfileNickname() override {}
  void RequestSyncLayout(const char*) override {}
  Roe<void> ForgetToolPermissions() override { return {}; }
  void OpenChangePin() override {}
  bool RecoveryOnOwnPage() const override { return on_own_page; }
  void OpenRecoveryPage() override {
    if (!on_own_page) {
      return; // as the controller: the desktop pushes no page
    }
    ++opened;
    page = "recovery";
    DataModelHost::Instance().Dirty("settings", "security_page");
  }
  void CloseRecoveryPage() override {
    ++closed;
    page = "";
    DataModelHost::Instance().Dirty("settings", "security_page");
  }
  bool on_own_page = true; // a phone; false = the desktop's inline block
  int opened = 0;
  int closed = 0;
  ui::String page;

private:
  SessionStore& store_;
  SettingsUiState state_;
};

constexpr const char* kContext = "recovery-phrase-page-test";

class RecoveryPhrasePageTest : public testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize();
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    root = std::filesystem::path(PP_BROWSER_ASSETS_DIR) / ".." / "logs" / "recovery_page_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "profile");
    BootstrapResult bootstrap{};
    bootstrap.config = Config::DefaultAppConfig();
    bootstrap.config_path = (root / "config.json").string();
    bootstrap.data_dir = root.string();
    bootstrap.profile_data_dir = (root / "profile").string();
    bootstrap.profile_prefs = UserPreferences::DefaultProfile();
    bootstrap.profile_prefs.pin_is_default = false;
    store.SetUiDemo(false);
    store.Initialize(std::move(bootstrap));

    std::string phrase;
    for (const std::string& word : kWords) {
      phrase += (phrase.empty() ? "" : " ") + word;
    }
    commands.recovery_phrase = [phrase](const SecretText&) -> Roe<std::optional<SecretText>> {
      return std::optional<SecretText>(SecretText(phrase));
    };
    pin_prompt.show_verify = [](const PinVerifyPurpose, PinGateCheck check, std::function<void()> on_ok) {
      check(SecretText("1234"), [on_ok = std::move(on_ok)](Roe<void>) { on_ok(); });
    };
    host = std::make_unique<FakeHost>(store);
    actions = std::make_unique<SettingsSecurityActions>(*host, commands, feedback, unlock, pin_prompt);
    ui::SetRenderInterface(&render);
    ASSERT_TRUE(ui::Initialise());
    ASSERT_TRUE(ui::LoadFontFace(std::string(PP_BROWSER_ASSETS_DIR) + "/fonts/Inter-Regular.ttf", true));
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext(kContext);
    }
    actions.reset();
    ui::Shutdown();
    ui::SetRenderInterface(nullptr);
    AppRuntime::Shutdown();
    std::filesystem::remove_all(root);
  }

  /** The phone page block of settings.rml in a `width` x `height` compact layer, on the light or dark colours. */
  void Open(const int width, const int height, const bool dark = false, const bool with_desktop_block = false) {
    context = ui::CreateContext(kContext, {width, height});
    ASSERT_NE(context, nullptr);
    ASSERT_TRUE(DataModelHost::Instance().Register(context, "settings", [this](ui::DataModelConstructor& ctor) {
      actions->Bind(ctor);
      ctor.Bind("security_page", &host->page);
    }));
    std::string rml = ReadAll("views/settings.rml");
    const size_t from = rml.find("<div class=\"recovery-page\"");
    const size_t to = rml.rfind("  </div>\n</div>");
    ASSERT_NE(from, std::string::npos);
    ASSERT_NE(to, std::string::npos);
    std::string block = rml.substr(from, to - from);
    const std::string marker = "{{include:settings_security_recovery}}";
    block.replace(block.find(marker), marker.size(), ReadAll("views/settings_security_recovery.rml"));
    if (with_desktop_block) {
      // The desktop's inline block of the real Security section, mounted beside the phone's page as in the real view.
      const std::string section = ReadAll("views/settings_section_security.rml");
      const size_t desktop_from = section.find("<div class=\"stack settings-desktop-only\" data-if=\"recovery_shown");
      const size_t desktop_done = section.find("{{i18n:settings.security.recovery.done}}");
      ASSERT_NE(desktop_from, std::string::npos);
      ASSERT_NE(desktop_done, std::string::npos);
      const size_t desktop_to = section.find("</div>", desktop_done) + 6;
      std::string desktop = section.substr(desktop_from, desktop_to - desktop_from);
      desktop.replace(desktop.find(marker), marker.size(), ReadAll("views/settings_security_recovery.rml"));
      block = desktop + block;
    }
    const std::string page = "<rml><head><style>" + ReadAll("themes/components.rcss") +
                             ReadAll(dark ? "themes/colors-dark.rcss" : "themes/colors-light.rcss") +
                             "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
                             "<div style=\"position: absolute; top: 0; left: 0; width: " +
                             std::to_string(width) + "dp; height: " + std::to_string(height) +
                             "dp;\"><div class=\"shell-layer shell-layer-base shell-layer-compact\">"
                             "<div class=\"settings-detail-view settings-subpage-view\" data-model=\"settings\">" +
                             LocalizationService::Instance().LocalizeText(block) + "</div></div></div></body></rml>";
    document = context->LoadDocumentFromMemory(page);
    ASSERT_NE(document, nullptr);
    document->Show();
    Update();
  }
  void Update() {
    AppRuntime::RunUITasks();
    context->Update();
    context->Update();
  }
  /** The Settings row's action: the PIN (stubbed), then the phrase page. */
  void ShowFromSettings() {
    actions->ShowRecoveryPhrase();
    AppRuntime::DrainWorkersThenUI();
    AppRuntime::RunUITasks();
    Update();
  }
  void Tap(const std::string& selector) {
    ui::ElementList found;
    document->QuerySelectorAll(found, selector);
    ASSERT_FALSE(found.empty()) << selector;
    found[0]->DispatchEvent(ui::EventId::Click, {});
    Update();
  }
  ui::ElementList All(const std::string& selector) {
    ui::ElementList found;
    document->QuerySelectorAll(found, selector);
    return found;
  }
  /** data-if only hides (display: none): what counts is what is visible. */
  size_t Visible(const std::string& selector) {
    size_t count = 0;
    for (ui::Element* element : All(selector)) {
      count += element->IsVisible() ? 1 : 0;
    }
    return count;
  }
  bool PageHoldsAWord() {
    const std::string text = document->GetInnerRML().c_str();
    return std::any_of(kWords.begin(), kWords.end(),
                       [&](const std::string& word) { return text.find(word) != std::string::npos; });
  }

  NullRender render;
  SessionStore store;
  SettingsCommands commands;
  ShellFeedbackPorts feedback;
  UnlockEnsurePorts unlock;
  PinPromptPorts pin_prompt;
  std::filesystem::path root;
  std::unique_ptr<FakeHost> host;
  std::unique_ptr<SettingsSecurityActions> actions;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
};

TEST_F(RecoveryPhrasePageTest, OpensCoveredWithNoWordInTheDocumentAndOneTapShowsAllInOrder) {
  Open(375, 667);
  EXPECT_TRUE(All(".recovery-card").empty()) << "nothing before the PIN";
  ShowFromSettings();
  EXPECT_EQ(host->opened, 1) << "the Settings entry opens the page";

  ASSERT_EQ(All(".recovery-card").size(), 24u);
  EXPECT_FALSE(PageHoldsAWord()) << "covered: no word text anywhere in the document";
  for (const std::string& word : actions->RecoveryWordsForTest()) {
    EXPECT_TRUE(word.empty()) << "and none bound";
  }
  const ui::ElementList numbers = All(".recovery-num");
  ASSERT_EQ(numbers.size(), 24u);
  for (size_t i = 0; i < 24; ++i) {
    EXPECT_EQ(std::string(numbers[i]->GetInnerRML().c_str()), std::to_string(i + 1));
  }
  EXPECT_EQ(Visible(".recovery-reveal"), 1u);

  Tap(".recovery-reveal");
  EXPECT_EQ(Visible(".recovery-reveal"), 0u) << "the control goes away";
  EXPECT_EQ(actions->RecoveryWordsForTest(), kWords);
  const ui::ElementList words = All(".recovery-word");
  std::vector<std::string> shown;
  for (ui::Element* word : words) {
    if (word->IsVisible()) {
      shown.emplace_back(word->GetInnerRML().c_str());
    }
  }
  EXPECT_EQ(shown, kWords) << "all 24, in order, in one tap";
}

TEST_F(RecoveryPhrasePageTest, TheBottomButtonClosesTheCoveredAndTheRevealedPageAndReopeningStartsCovered) {
  Open(375, 667);
  ShowFromSettings();
  Tap(".recovery-done"); // covered state
  EXPECT_EQ(host->closed, 1);
  EXPECT_FALSE(actions->RecoveryPhraseShown());
  EXPECT_TRUE(All(".recovery-card").empty());
  EXPECT_FALSE(PageHoldsAWord());

  ShowFromSettings();
  Tap(".recovery-reveal");
  EXPECT_TRUE(PageHoldsAWord());
  Tap(".recovery-done"); // revealed state
  EXPECT_EQ(host->closed, 2);
  EXPECT_TRUE(actions->RecoveryWordsForTest().empty()) << "the bound words are cleared";
  EXPECT_FALSE(PageHoldsAWord());

  ShowFromSettings(); // reopened: covered again
  EXPECT_EQ(All(".recovery-card").size(), 24u);
  EXPECT_FALSE(PageHoldsAWord());
  EXPECT_EQ(Visible(".recovery-reveal"), 1u);
}

TEST_F(RecoveryPhrasePageTest, LeavingByBackOrTheBackgroundClearsTheWordsToo) {
  Open(375, 667);
  ShowFromSettings();
  Tap(".recovery-reveal");
  actions->HideRecoveryPhrase(); // what the page's back / swipe / the app's background all end in
  Update();
  EXPECT_TRUE(actions->RecoveryWordsForTest().empty());
  EXPECT_FALSE(PageHoldsAWord());
}

// data-if only hides, so what counts is which nodes exist: the grid of the block that is not in use must not be mounted.
TEST_F(RecoveryPhrasePageTest, OnAPhoneTheRevealedWordsAreBoundOnceNotAlsoInTheDesktopBlock) {
  Open(375, 667, false, /*with_desktop_block=*/true);
  ShowFromSettings();
  Tap(".recovery-reveal");
  EXPECT_EQ(All(".recovery-card").size(), 24u) << "one grid in the whole combined document";
  EXPECT_EQ(All(".recovery-page .recovery-card").size(), 24u) << "the phone's page holds it";
  EXPECT_EQ(All(".settings-desktop-only .recovery-card").size(), 0u) << "the desktop block holds none";
  const std::string text = document->GetInnerRML().c_str();
  for (const char* word : {"mushroom", "wrestle", "wisdom"}) {
    size_t copies = 0;
    for (size_t at = text.find(std::string(">") + word + "<"); at != std::string::npos;
         at = text.find(std::string(">") + word + "<", at + 1)) {
      ++copies;
    }
    EXPECT_EQ(copies, 1u) << word << " is in the document once";
  }
}

TEST_F(RecoveryPhrasePageTest, OnTheDesktopTheInlineBlockHoldsTheWordsAndThePageHoldsNone) {
  host->on_own_page = false;
  Open(375, 667, false, /*with_desktop_block=*/true);
  ShowFromSettings();
  EXPECT_EQ(host->opened, 0) << "no page is pushed on the desktop";
  EXPECT_EQ(All(".settings-desktop-only .recovery-card").size(), 24u);
  EXPECT_EQ(All(".recovery-page .recovery-card").size(), 0u);
  Tap(".recovery-reveal");
  EXPECT_EQ(actions->RecoveryWordsForTest(), kWords);
  EXPECT_EQ(All(".recovery-card").size(), 24u) << "still one grid";
  Tap(".settings-desktop-only .btn-primary");
  EXPECT_TRUE(All(".recovery-card").empty());
  EXPECT_TRUE(actions->RecoveryWordsForTest().empty());
}

class RecoveryPhraseLayoutTest : public RecoveryPhrasePageTest,
                                 public testing::WithParamInterface<std::pair<int, bool>> {};

TEST_P(RecoveryPhraseLayoutTest, ThreeCardsToARowReadingOrderNothingOverflowsAndTheButtonStaysPut) {
  const auto [width, dark] = GetParam();
  Open(width, 667, dark);
  ShowFromSettings();
  Tap(".recovery-reveal");
  const ui::ElementList cards = All(".recovery-card");
  ASSERT_EQ(cards.size(), 24u);
  float row_top = -1.f;
  int row = -1;
  float last_left = 0.f;
  for (size_t i = 0; i < cards.size(); ++i) {
    const ui::Vector2f at = cards[i]->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f size = cards[i]->GetBox().GetSize(ui::BoxArea::Border);
    if (static_cast<int>(i % 3) == 0) {
      EXPECT_GT(at.y, row_top) << "card " << i + 1 << " starts a new row";
      row_top = at.y;
      ++row;
    } else {
      EXPECT_FLOAT_EQ(at.y, row_top) << "card " << i + 1 << " is in its row";
      EXPECT_GT(at.x, last_left) << "left to right";
    }
    last_left = at.x;
    EXPECT_GE(at.x, 0.f);
    EXPECT_LE(at.x + size.x, static_cast<float>(width)) << "card " << i + 1 << " inside the page width";
  }
  EXPECT_EQ(row, 7) << "eight rows of three";

  for (ui::Element* word : All(".recovery-word")) {
    if (word->IsVisible()) {
      EXPECT_LE(word->GetScrollWidth(), word->GetClientWidth() + 0.5f) << word->GetInnerRML().c_str() << " clipped";
      EXPECT_LT(word->GetClientHeight(), 24.f) << word->GetInnerRML().c_str() << " wrapped";
    }
  }
  // The number slots are one width, so the words line up in a column.
  const ui::ElementList numbers = All(".recovery-num");
  for (ui::Element* number : numbers) {
    EXPECT_FLOAT_EQ(number->GetBox().GetSize(ui::BoxArea::Border).x, 16.f);
  }
  // The button is pinned inside the page, at least 44dp tall, and the scroller does not reach under it.
  ui::ElementList done = All(".recovery-done");
  ASSERT_EQ(done.size(), 1u);
  const ui::Vector2f at = done[0]->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Vector2f size = done[0]->GetBox().GetSize(ui::BoxArea::Border);
  EXPECT_GE(size.y, 44.f);
  EXPECT_LE(at.y + size.y, 667.f);
  EXPECT_GE(at.x, 0.f);
  EXPECT_LE(at.x + size.x, static_cast<float>(width));
  // The scroller is mounted by its slot (data-for): the template element stays in the DOM, hidden.
  ASSERT_EQ(Visible(".recovery-scroll"), 1u);
  for (ui::Element* scroll : All(".recovery-scroll")) {
    if (scroll->IsVisible()) {
      EXPECT_LE(scroll->GetAbsoluteOffset(ui::BoxArea::Border).y + scroll->GetBox().GetSize(ui::BoxArea::Border).y,
                at.y + 0.5f);
    }
  }
  EXPECT_EQ(All(".recovery-lead").size(), 1u);
  EXPECT_EQ(All(".page-large-title").size(), 0u) << "one title per page, and it is the bar's";
}

INSTANTIATE_TEST_SUITE_P(Widths, RecoveryPhraseLayoutTest,
                         testing::Values(std::make_pair(375, false), std::make_pair(393, false),
                                         std::make_pair(375, true), std::make_pair(393, true)));

TEST(RecoveryPhraseMarkupTest, TheEntryAndTheDesktopUseTheSharedGridAndTheDarkThemeStylesIt) {
  const std::string security = ReadAll("views/settings_section_security.rml");
  const std::string settings = ReadAll("views/settings.rml");
  EXPECT_NE(security.find("data-event-click=\"show_recovery_phrase()\""), std::string::npos);
  EXPECT_NE(security.find("{{include:settings_security_recovery}}"), std::string::npos) << "desktop: inline";
  EXPECT_NE(settings.find("{{include:settings_security_recovery}}"), std::string::npos) << "phones: the page";
  EXPECT_NE(settings.find("security_page == 'recovery'"), std::string::npos);
  for (const char* theme : {"themes/colors-light.rcss", "themes/colors-dark.rcss"}) {
    const std::string css = ReadAll(theme);
    EXPECT_NE(css.find(".recovery-card"), std::string::npos) << theme;
    EXPECT_NE(css.find(".recovery-word"), std::string::npos) << theme;
  }
  const std::string grid = ReadAll("views/settings_security_recovery.rml");
  EXPECT_EQ(grid.find("{{item.word}}", grid.find("{{item.word}}") + 1), std::string::npos) << "bound once";
}

} // namespace
} // namespace pbr
