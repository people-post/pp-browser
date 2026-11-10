#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

namespace fs = std::filesystem;

// What the user reads comes from the locale files (Tr / Localized), never from a literal in the code (#178): a
// literal handed straight to a toast, a dialog or `WithUser` shows English in a Chinese interface.
const char* const kCalls[] = {
    "UserFeedback::Ok(",    "UserFeedback::Fail(",    "UserFeedback::Note(", "UserFeedback::NeedsSetup(",
    "UserFeedback::Alert(", "UserFeedback::Confirm(", "ShowToast(",          ".WithUser("};

bool IsSource(const fs::path& path) {
  const std::string ext = path.extension().string();
  return ext == ".cpp" || ext == ".mm" || ext == ".h";
}

bool InTests(const fs::path& path) {
  for (const auto& part : path) {
    if (part == "tests" || part == "testing") {
      return true;
    }
  }
  return false;
}

TEST(UserTextSourceTest, NoLiteralIsHandedToTheUserWithoutTheLocaleFiles) {
  const fs::path root = PP_SOURCE_DIR;
  std::vector<std::string> offenders;
  for (const char* dir : {"src", "capabilities"}) {
    for (const auto& entry : fs::recursive_directory_iterator(root / dir)) {
      if (!entry.is_regular_file() || !IsSource(entry.path()) || InTests(entry.path())) {
        continue;
      }
      std::ifstream in(entry.path());
      std::stringstream buffer;
      buffer << in.rdbuf();
      const std::string text = buffer.str();
      for (const char* call : kCalls) {
        for (size_t at = text.find(call); at != std::string::npos; at = text.find(call, at + 1)) {
          size_t i = at + std::string(call).size();
          while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
          }
          if (i < text.size() && text[i] == '"') {
            offenders.push_back(fs::relative(entry.path(), root).string() + ": " + call);
          }
        }
      }
    }
  }
  EXPECT_TRUE(offenders.empty()) << "a literal reaches the user; use Tr() / Localized():\n"
                                 << [&] {
                                      std::string joined;
                                      for (const std::string& offender : offenders) {
                                        joined += offender + "\n";
                                      }
                                      return joined;
                                    }();
}

} // namespace
} // namespace pbr
