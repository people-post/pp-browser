#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

// B78: `data-rml` sets inner RML, so a bound value is parsed as markup and `{{…}}` inside it is evaluated as
// a data expression. Names, titles and message text come from other people, so plain text must be bound
// with `{{expr}}` (set as text) and `data-rml` is reserved for fields that really carry markup — by
// convention those are named `*_rml`. The markup src/ serializes (ShellHost, ContextMenuHost) is covered by
// scripts/check/check_rml_bindings.sh in CI lint, which this test mirrors for the shipped views.
namespace {

std::vector<std::filesystem::path> RmlFiles() {
  std::vector<std::filesystem::path> files;
  for (const char* dir : {"views", "samples"}) {
    const std::filesystem::path root = std::filesystem::path(PP_BROWSER_ASSETS_DIR) / dir;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
      if (entry.is_regular_file() && entry.path().extension() == ".rml") {
        files.push_back(entry.path());
      }
    }
  }
  return files;
}

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

} // namespace

TEST(RmlBindingGuardTest, DataRmlIsOnlyUsedForMarkupFields) {
  // Either quote style.
  const std::regex binding("data-rml=[\"']([^\"']*)[\"']");
  std::vector<std::string> offenders;
  size_t files = 0;
  for (const std::filesystem::path& path : RmlFiles()) {
    ++files;
    const std::string text = ReadAll(path);
    for (auto it = std::sregex_iterator(text.begin(), text.end(), binding); it != std::sregex_iterator(); ++it) {
      const std::string expr = (*it)[1];
      if (expr.size() < 4 || expr.compare(expr.size() - 4, 4, "_rml") != 0) {
        offenders.push_back(path.filename().string() + ": data-rml=\"" + expr + "\"");
      }
    }
  }
  ASSERT_GT(files, 0u) << "no .rml files under " << PP_BROWSER_ASSETS_DIR;
  EXPECT_TRUE(offenders.empty()) << "plain text must be bound as {{expr}}, not data-rml:\n"
                                 << [&] {
                                      std::string joined;
                                      for (const std::string& line : offenders) {
                                        joined += "  " + line + "\n";
                                      }
                                      return joined;
                                    }();
}

// The AI drawer's "you" row binds the avatar and name from the settings model (each element declares it) and the status
// line from two window-model fields (ShellHost::SetChatsLink), which show only while the state is non-empty.
TEST(RmlBindingGuardTest, TheAiDrawerMeRowResolvesItsNamesAndTheLinkStatus) {
  const std::string rml = ReadAll(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views" / "ai_drawer.rml");
  const size_t begin = rml.find("<button class=\"ai-drawer-me\"");
  ASSERT_NE(begin, std::string::npos);
  const std::string row = rml.substr(begin);
  EXPECT_NE(row.find("<div class=\"ai-drawer-me-avatar\" data-model=\"settings\""), std::string::npos);
  EXPECT_NE(row.find("<p class=\"ai-drawer-me-name\" data-model=\"settings\">{{profile_nickname}}</p>"),
            std::string::npos);
  const std::string settings = ReadAll(std::filesystem::path(PP_SOURCE_DIR) / "src/gui/SettingsController.cpp");
  for (const char* name :
       {"profile_has_icon", "profile_avatar_tone", "profile_icon_src", "profile_avatar_letter", "profile_nickname"}) {
    EXPECT_NE(row.find(name), std::string::npos) << name << " used by the row";
    EXPECT_NE(settings.find(std::string("ctor.Bind(\"") + name + "\""), std::string::npos)
        << name << " is bound in the settings model";
  }
  EXPECT_NE(row.find("class=\"ai-drawer-me-link\" data-if=\"chats_link_state != ''\""), std::string::npos);
  EXPECT_NE(row.find("{{chats_link_text}}"), std::string::npos);
  EXPECT_NE(row.find("cloud-ok.svg"), std::string::npos);
  EXPECT_NE(row.find("cloud-off.svg"), std::string::npos);
  const std::string host = ReadAll(std::filesystem::path(PP_SOURCE_DIR) / "src/gui/shell/ShellHost.cpp");
  EXPECT_NE(host.find("ctor.Bind(\"chats_link_state\""), std::string::npos);
  EXPECT_NE(host.find("ctor.Bind(\"chats_link_text\""), std::string::npos);
}

// The drawer's "you" row is 76dp: the stylesheet's row, the spacer under the list and ShellHost's constant agree.
TEST(RmlBindingGuardTest, TheAiDrawerMeRowHeightIsOneNumber) {
  const std::string css = ReadAll(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "themes" / "components.rcss");
  const std::string host = ReadAll(std::filesystem::path(PP_SOURCE_DIR) / "src/gui/shell/ShellHost.cpp");
  EXPECT_NE(host.find("kAiDrawerMeHeightDp = 76.f"), std::string::npos);
  const size_t row = css.find(".ai-drawer-me {");
  const size_t spacer = css.find(".ai-drawer-list-end {");
  ASSERT_NE(row, std::string::npos);
  ASSERT_NE(spacer, std::string::npos);
  EXPECT_NE(css.substr(row, 400).find("min-height: 76dp;"), std::string::npos);
  EXPECT_NE(css.substr(spacer, 120).find("height: 76dp;"), std::string::npos);
  const size_t avatar = css.find(".ai-drawer-me-avatar {");
  ASSERT_NE(avatar, std::string::npos);
  EXPECT_NE(css.substr(avatar, 260).find("width: 56dp;"), std::string::npos);
}

// The drawer's "you" row tap is the window model's event (ShellHost registers open_settings_from_avatar there; the
// drawer is not inside the page's shell model).
TEST(RmlBindingGuardTest, TheDrawerMeRowTapIsHandledByTheWindowModel) {
  const std::string rml = ReadAll(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views" / "ai_drawer.rml");
  EXPECT_NE(rml.find("id=\"ai-drawer-me\" type=\"button\" data-event-click=\"open_settings_from_avatar()\""),
            std::string::npos);
  const std::string host = ReadAll(std::filesystem::path(PP_SOURCE_DIR) / "src/gui/shell/ShellHost.cpp");
  const size_t window_model = host.find("Register(context, \"window\"");
  const size_t event = host.find("BindEventCallback(\"open_settings_from_avatar\"");
  ASSERT_NE(window_model, std::string::npos);
  ASSERT_NE(event, std::string::npos);
  EXPECT_LT(window_model, event);
  EXPECT_GT(host.find("Register(context, ", window_model + 1), event) << "inside the window model's constructor";
}

// The round back on New chat and Add contact is the shell's transient chrome (window model, transient_back), and the
// pages' own title bar (a pane-nav drawn over it) lets taps through: on the phone the back did nothing (2026-11-07).
TEST(RmlBindingGuardTest, NewChatAndAddContactBackReachTheShellsTransientBack) {
  const std::string layout = ReadAll(std::filesystem::path(PP_SOURCE_DIR) / "src/gui/shell/ShellLayoutRml.cpp");
  const size_t layer = layout.find("id=\\\"shell-transient-layer\\\" data-model=\\\"window\\\"");
  const size_t back = layout.find("data-event-click=\\\"transient_back()\\\"");
  ASSERT_NE(layer, std::string::npos);
  ASSERT_NE(back, std::string::npos);
  EXPECT_LT(layer, back) << "the back button sits in the window-model layer";
  const std::string host = ReadAll(std::filesystem::path(PP_SOURCE_DIR) / "src/gui/shell/ShellHost.cpp");
  const size_t window_model = host.find("Register(context, \"window\"");
  const size_t event = host.find("BindEventCallback(\"transient_back\"");
  ASSERT_NE(event, std::string::npos);
  EXPECT_LT(window_model, event);
  for (const char* view : {"new_chat.rml", "add_contact.rml"}) {
    const std::string rml = ReadAll(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views" / view);
    EXPECT_NE(rml.find("class=\"pane-nav nc-bar"), std::string::npos) << view << ": its bar is the nc-bar";
  }
  const std::string css = ReadAll(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "themes" / "components.rcss");
  EXPECT_NE(css.find(".shell-layer-compact .nc-bar {\n  pointer-events: none;\n}"), std::string::npos)
      << "the bar over the shell's back lets taps through";
}

// Kenneth 2026-11-07: the AI drawer's last row was hidden behind the "you" bar (the engine leaves a scroller's bottom
// padding out of its scroll height): an end spacer, sized by ShellHost for the bar and the home indicator. The list
// draws 20 conversations and 20 more near its end (SessionListWindow, like the chats list).
TEST(RmlBindingGuardTest, TheAiDrawerListEndsWithASpacerAndGrowsOnScroll) {
  const std::string rml = ReadAll(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views" / "ai_drawer.rml");
  const size_t list = rml.find("id=\"ai-drawer-sessions\"");
  const size_t rows = rml.find("data-for=\"session : ai_sessions\"");
  const size_t spacer = rml.find("id=\"ai-drawer-list-end\"");
  const size_t me = rml.find("id=\"ai-drawer-me\"");
  ASSERT_NE(list, std::string::npos);
  ASSERT_NE(spacer, std::string::npos);
  ASSERT_NE(me, std::string::npos);
  EXPECT_LT(rows, spacer);
  EXPECT_LT(spacer, me) << "the spacer is the list's last child, before the bar";
  EXPECT_NE(rml.substr(list, rml.find('>', list) - list).find("data-event-scroll=\"ai_sessions_scroll()\""),
            std::string::npos);
  const std::string host = ReadAll(std::filesystem::path(PP_SOURCE_DIR) / "src/gui/shell/ShellHost.cpp");
  EXPECT_NE(host.find("GetElementById(\"ai-drawer-list-end\"), \"height\""), std::string::npos);
  EXPECT_NE(host.find("GetElementById(\"ai-drawer-me\"), \"padding-bottom\""), std::string::npos);
  const std::string controller = ReadAll(std::filesystem::path(PP_SOURCE_DIR) / "src/gui/chat/ChatController.cpp");
  EXPECT_NE(controller.find("BindEventCallback(\"ai_sessions_scroll\""), std::string::npos);
}

// Kenneth 2026-11-07: an AI conversation's header showed a fixed star; it shows its list row's letter and colour.
TEST(RmlBindingGuardTest, TheChatHeaderShowsTheRowsGlyphForAnAiConversation) {
  const std::string rml = ReadAll(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views" / "chat.rml");
  const size_t start = rml.find("<div class=\"chat-header-avatar\"");
  ASSERT_NE(start, std::string::npos);
  const std::string avatar = rml.substr(start, rml.find("<div class=\"chat-header-text\">", start) - start);
  EXPECT_EQ(avatar.find("sparkle"), std::string::npos);
  EXPECT_EQ(avatar.find("!thread_is_ai && !thread_is_group && thread_avatar_tone"), std::string::npos);
  EXPECT_NE(avatar.find("data-if=\"!thread_is_group && !thread_has_icon\">{{thread_avatar_letter}}"),
            std::string::npos);
  const std::string chrome = ReadAll(std::filesystem::path(PP_SOURCE_DIR) / "src/gui/chat/ChatThreadChrome.cpp");
  EXPECT_NE(chrome.find("MakeAvatarGlyph(label.title, label.stable_id)"), std::string::npos)
      << "the row's own inputs (ChatPresenter)";
}
