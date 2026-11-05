// One leading column on every phone list (Kenneth 2026-11-04, PLAN-ui-refresh-impl 68): person avatars are 44dp, the
// settings / Me icon tiles 36dp centred in the same 44dp, and the text starts 16 + 44 + 12 = 72dp into the row.
#include <gtest/gtest.h>

#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace pbr {
namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  std::string folded;
  folded.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
      continue;
    }
    folded += text[i];
  }
  return folded;
}

const std::string& Components() {
  static const std::string text = ReadFile(std::string(PP_BROWSER_ASSETS_DIR) + "/themes/components.rcss");
  return text;
}

/** The body of the first rule whose whole selector is `selector` (not one entry of a longer selector list). */
std::optional<std::string> Rule(const std::string& selector) {
  const std::string& css = Components();
  size_t at = css.find("\n" + selector + " {\n");
  // The selector starts after a blank line or a comment, not after another selector's comma.
  while (at != std::string::npos && at > 0 && css[at - 1] == ',') {
    at = css.find("\n" + selector + " {\n", at + 1);
  }
  if (at == std::string::npos) {
    return std::nullopt;
  }
  const size_t open = css.find('{', at);
  const size_t close = css.find("\n}", open);
  return css.substr(open + 1, close - open);
}

bool Has(const std::optional<std::string>& rule, const std::string& declaration) {
  return rule && rule->find("  " + declaration + ";") != std::string::npos;
}

TEST(ListLeadingColumnTest, PersonAvatarsAre44dp) {
  for (const char* selector : {".shell-layer-compact .chat-row-avatar", ".shell-layer-compact .contacts-row-avatar-frame",
                               ".shell-layer-compact .home-chat-av"}) {
    const auto rule = Rule(selector);
    ASSERT_TRUE(rule.has_value()) << selector;
    EXPECT_TRUE(Has(rule, "width: 44dp")) << selector;
    EXPECT_TRUE(Has(rule, "height: 44dp")) << selector;
    EXPECT_TRUE(Has(rule, "border-radius: 22dp")) << selector;
  }
  // Group members (group info, choose the next owner): the avatar is 44dp too.
  const auto members = Rule(".cd-row-avatar,\n.cd-row-plus");
  EXPECT_TRUE(Has(members, "width: 44dp"));
  EXPECT_TRUE(Has(members, "height: 44dp"));
}

TEST(ListLeadingColumnTest, IconTilesAre36dpCentredIn44dp) {
  const auto tile = Rule(".shell-layer-compact .group-icon");
  ASSERT_TRUE(tile.has_value());
  EXPECT_TRUE(Has(tile, "width: 36dp"));
  EXPECT_TRUE(Has(tile, "height: 36dp"));
  // 4dp + 36dp + 4dp = the 44dp column, then the 12dp gap: 16dp to the text.
  EXPECT_TRUE(Has(tile, "margin: 0 16dp 0 4dp"));
  const auto plus = Rule(".cd-row-plus");
  EXPECT_TRUE(Has(plus, "width: 36dp"));
  EXPECT_TRUE(Has(plus, "margin: 0 16dp 0 4dp"));
}

TEST(ListLeadingColumnTest, TextStartsAt72dpAndRowsStayTall) {
  // Rows start 16dp in and leave 12dp after the 44dp column.
  for (const char* selector : {".shell-layer-compact .sidebar-session", ".shell-layer-compact .contacts-row"}) {
    const auto rule = Rule(selector);
    ASSERT_TRUE(rule.has_value()) << selector;
    EXPECT_TRUE(Has(rule, "padding: 0 0 0 16dp")) << selector;
    EXPECT_TRUE(Has(rule, "gap: 12dp")) << selector;
  }
  EXPECT_TRUE(Has(Rule(".shell-layer-compact .group-row"), "padding: 0 0 0 16dp"));
  EXPECT_TRUE(Has(Rule(".cd-row"), "padding: 0 0 0 16dp"));
  EXPECT_TRUE(Has(Rule(".cd-row.cd-row--member"), "gap: 12dp"));
  EXPECT_TRUE(Has(Rule(".shell-layer-compact .home-chat-av"), "margin-right: 12dp"));
  // At least 50dp tall.
  EXPECT_TRUE(Has(Rule(".shell-layer-compact .group-row"), "min-height: 50dp"));
  EXPECT_TRUE(Has(Rule(".cd-row.cd-row--member"), "height: 60dp"));
  EXPECT_TRUE(Has(Rule(".shell-layer-compact .contacts-row"), "height: 60dp"));
}

} // namespace
} // namespace pbr
