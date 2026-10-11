#include "foundation/platform/os/OsProcess.h"

#include <gtest/gtest.h>

#include <string>

#if !defined(_WIN32)

#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace {

std::string ReadAll(pbr::os::OsProcessPipe& process) {
  std::string out;
  char buffer[256];
  for (int n = process.Read(buffer, sizeof(buffer), 5000); n > 0; n = process.Read(buffer, sizeof(buffer), 5000)) {
    out.append(buffer, static_cast<size_t>(n));
  }
  return out;
}

} // namespace

// PB-036: a child gets only its three standard streams and none of our PP_BROWSER_* variables (the PIN, the
// config path); everything else in the environment, which an MCP server may need, stays.
TEST(OsProcessTest, ChildInheritsNeitherOurFdsNorOurVariables) {
  setenv("PP_BROWSER_PIN", "1017", 1);
  setenv("PP_TEST_KEEP_ME", "yes", 1);
  const int spare = open("/dev/null", O_RDONLY);
  ASSERT_GE(spare, 0);
  ASSERT_EQ(dup2(spare, 9), 9);

  pbr::os::OsProcessPipe process;
  ASSERT_TRUE(process.Start(
      "/bin/sh", {"-c", "echo pin=${PP_BROWSER_PIN:-unset} keep=${PP_TEST_KEEP_ME:-unset}; "
                        "if [ -e /dev/fd/9 ]; then echo fd9=open; else echo fd9=closed; fi"}));
  const std::string out = ReadAll(process);
  process.Stop();
  close(spare);
  close(9);
  unsetenv("PP_BROWSER_PIN");
  unsetenv("PP_TEST_KEEP_ME");

  EXPECT_NE(out.find("pin=unset"), std::string::npos) << out;
  EXPECT_NE(out.find("keep=yes"), std::string::npos) << out;
  EXPECT_NE(out.find("fd9=closed"), std::string::npos) << out;
}

#endif
