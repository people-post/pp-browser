// The status popover's "Open network settings" link: the network rows now live on the Advanced page, which is hidden
// until the five taps on the version, so the link is offered only while Advanced settings are shown.
#include "feature/conversations/MessagingShellPorts.h"
#include "gui/shell/ShellStatusbar.h"
#include "lib/ui/ShellTypes.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

} // namespace

TEST(ShellStatusbarAdvancedTest, TheLinkFollowsTheAdvancedFlag) {
  ShellState state;
  ui::Context* context = nullptr;
  MessagingShellPorts ports;
  ShellStatusbar bar(state, context, ports, []() {});
  EXPECT_FALSE(state.statusbar_popover_advanced) << "off until the flag says otherwise";
  bar.SetAdvanced(true);
  EXPECT_TRUE(state.statusbar_popover_advanced);
  bar.SetAdvanced(false);
  EXPECT_FALSE(state.statusbar_popover_advanced);
}

TEST(ShellStatusbarAdvancedTest, OnlyTheNetworkSettingsButtonIsGated) {
  const std::string rml = ReadAsset("samples/window_shell.rml");
  ASSERT_FALSE(rml.empty());
  const size_t link = rml.find("data-event-click=\"open_network_settings()\"");
  ASSERT_NE(link, std::string::npos);
  const size_t button = rml.rfind("<button", link);
  ASSERT_NE(button, std::string::npos);
  EXPECT_NE(rml.substr(button, link - button).find("data-if=\"statusbar_popover_advanced\""), std::string::npos);
  // The retest button next to it stays for everyone.
  const size_t retest = rml.find("data-event-click=\"retest_statusbar_reachability()\"");
  ASSERT_NE(retest, std::string::npos);
  const size_t retest_button = rml.rfind("<button", retest);
  EXPECT_EQ(rml.substr(retest_button, retest - retest_button).find("data-if="), std::string::npos);
}

} // namespace pbr
