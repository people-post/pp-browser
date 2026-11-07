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
  EXPECT_TRUE(offenders.empty()) << "plain text must be bound as {{expr}}, not data-rml:\n" << [&] {
    std::string joined;
    for (const std::string& line : offenders) {
      joined += "  " + line + "\n";
    }
    return joined;
  }();
}

// Every name the Chats bar binds must be registered in the model its element declares (the bar's avatar and name live
// in the settings model; the rest of the bar in the shell/window ones). A name nobody binds renders as nothing.
TEST(RmlBindingGuardTest, TheChatsBarAvatarAndNameResolveInTheSettingsModel) {
  const std::string rml = ReadAll(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views" / "sidebar.rml");
  const size_t begin = rml.find("<div class=\"pane-nav\">");
  const size_t end = rml.find("<div class=\"sidebar-header row\">");
  ASSERT_NE(begin, std::string::npos);
  ASSERT_NE(end, std::string::npos);
  const std::string bar = rml.substr(begin, end - begin);
  // The avatar element carries data-model="settings" itself, like the AI drawer's row, and the name span too.
  EXPECT_NE(bar.find("<div class=\"shell-circle-avatar\" data-model=\"settings\""), std::string::npos);
  EXPECT_NE(bar.find("<p class=\"pane-nav-avatar-name\" data-model=\"settings\">{{profile_nickname}}</p>"), std::string::npos);
  const std::string settings = ReadAll(std::filesystem::path(PP_SOURCE_DIR) / "src/gui/SettingsController.cpp");
  for (const char* name : {"profile_has_icon", "profile_avatar_tone", "profile_icon_src", "profile_avatar_letter",
                           "profile_nickname"}) {
    EXPECT_NE(bar.find(name), std::string::npos) << name << " used by the bar";
    EXPECT_NE(settings.find(std::string("ctor.Bind(\"") + name + "\""), std::string::npos)
        << name << " is bound in the settings model";
  }
}

// The Chats avatar's tap is the window model's event (ShellHost registers open_settings_from_avatar there); inside the
// page's shell model it found no handler and the tap did nothing on the phone. The button declares the window model.
TEST(RmlBindingGuardTest, TheChatsAvatarTapIsHandledByTheWindowModel) {
  const std::string rml = ReadAll(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views" / "sidebar.rml");
  EXPECT_NE(rml.find("<button class=\"pane-nav-avatar\" type=\"button\" data-model=\"window\" "
                     "data-event-click=\"open_settings_from_avatar()\">"),
            std::string::npos);
  const std::string host = ReadAll(std::filesystem::path(PP_SOURCE_DIR) / "src/gui/shell/ShellHost.cpp");
  const size_t window_model = host.find("Register(context, \"window\"");
  const size_t event = host.find("BindEventCallback(\"open_settings_from_avatar\"");
  ASSERT_NE(window_model, std::string::npos);
  ASSERT_NE(event, std::string::npos);
  EXPECT_LT(window_model, event);
  EXPECT_GT(host.find("Register(context, ", window_model + 1), event) << "inside the window model's constructor";
}
