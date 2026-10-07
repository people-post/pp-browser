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
// convention those are named `*_rml`.
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
  const std::regex binding("data-rml=\"([^\"]*)\"");
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
