// Regression guard: every CSS class the AI-reply renderers emit must have a rule in assets/themes.
// A renderer once emitted `chat-long-list-meta-end` after its rule was lost in an edit; the RML-only
// tests passed and only a device screenshot showed it. The renderer sources are scanned at test time,
// so a new class is checked automatically.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void AddClassTokens(const std::string& value, std::set<std::string>& out) {
  std::istringstream tokens(value);
  std::string token;
  while (tokens >> token) {
    if (token.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") == std::string::npos) {
      out.insert(token);
    }
  }
}

// Classes in `class=\"...\"` literals, plus `classes = "..."` / `classes += "..."` string builders.
std::set<std::string> EmittedClasses(const std::string& source) {
  std::set<std::string> out;
  const std::regex attr(R"(class=\\"([^"\\]*)\\")");
  for (std::sregex_iterator it(source.begin(), source.end(), attr), end; it != end; ++it) {
    AddClassTokens((*it)[1], out);
  }
  const std::regex builder(R"re(\bclasses\s*\+?=\s*"([^"]*)")re");
  for (std::sregex_iterator it(source.begin(), source.end(), builder), end; it != end; ++it) {
    AddClassTokens((*it)[1], out);
  }
  return out;
}

} // namespace

TEST(RenderedClassesHaveStylesTest, EveryEmittedClassHasAThemeRule) {
  const fs::path root = PP_BROWSER_SOURCE_ROOT;
  std::string css;
  for (const auto& entry : fs::directory_iterator(root / "assets/themes")) {
    if (entry.path().extension() == ".rcss") {
      css += ReadFile(entry.path()) + "\n";
    }
  }
  ASSERT_FALSE(css.empty()) << "no .rcss files under " << (root / "assets/themes");

  std::set<std::string> emitted;
  for (const char* file : {"src/domain/ai/StructuredTextParser.cpp", "src/domain/ai/MarkdownToRml.cpp"}) {
    const std::string source = ReadFile(root / file);
    ASSERT_FALSE(source.empty()) << file;
    const auto classes = EmittedClasses(source);
    emitted.insert(classes.begin(), classes.end());
  }
  ASSERT_GT(emitted.size(), 20u) << "class scan found suspiciously few classes";

  // Classes that are intentionally unstyled (hooks for scripts or tests). Keep this empty unless
  // a class really needs no rule, and say why next to the entry.
  // These four were already emitted without any rule when this test was added (they inherit the
  // parent's look); style them and delete the entry, or drop the class from the renderer.
  const std::set<std::string> kUnstyled = {
      "chat-action-list-title", "chat-key-value-value", "chat-poll-question", "working-set-calendar",
  };

  for (const std::string& name : emitted) {
    if (kUnstyled.count(name) != 0) {
      continue;
    }
    // A trailing '-' means the literal is a prefix completed at runtime (chat-card-<variant>):
    // at least one rule must start with it.
    const std::string pattern = name.back() == '-' ? "\\." + name + "[a-z0-9-]*" : "\\." + name + R"((?![a-z0-9-]))";
    EXPECT_TRUE(std::regex_search(css, std::regex(pattern))) << "class '" << name << "' is emitted but no theme rule styles it";
  }
}
