// The status bar is drawn inside SDL's event watch during a live window resize (BrowserHost's LiveResizeEventWatch). What
// it reads must never wait for the mesh's io lock: the pump holds that lock for a whole turn and may post to the UI from
// it, which needs SDL's watch lock — a deadlock (#816). Its ports are read from their source, as the other wiring tests
// do: a WithIoLock (or a link-table read, which needs the lock) brought back into them fails here.
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

TEST(StatusbarIoLockWiringTest, TheStatusbarPortsTakeNoIoLock) {
  const std::string source = ReadFile(std::string(PP_SOURCE_DIR) +
                                      "/capabilities/messaging/src/feature/conversations/MessagingShellPorts.cpp");
  ASSERT_FALSE(source.empty()) << "MessagingShellPorts.cpp not found";
  EXPECT_EQ(source.find("WithIoLock"), std::string::npos) << "the status bar must not wait for the io lock";
  EXPECT_EQ(source.find("RunAndWait"), std::string::npos) << "nor run a task on the lane and wait for it";
  EXPECT_EQ(source.find(".Links()"), std::string::npos) << "the link table is read under the io lock";
  EXPECT_NE(source.find("hub.MeshLinkCount()"), std::string::npos) << "the link count is the one the lane published";
}

TEST(StatusbarIoLockWiringTest, TheLinkCountIsPublishedFromTheLane) {
  const std::string source =
      ReadFile(std::string(PP_SOURCE_DIR) + "/capabilities/messaging/src/feature/conversations/ConversationsHub.cpp");
  ASSERT_FALSE(source.empty()) << "ConversationsHub.cpp not found";
  const size_t start = source.find("void ConversationsHub::StartMeshLinkCountTick()");
  ASSERT_NE(start, std::string::npos);
  const std::string body = source.substr(start, source.find("\n}\n", start) - start);
  EXPECT_NE(body.find("AddIoDueTick"), std::string::npos) << "counted on the io lane, where the lock is held";
  EXPECT_EQ(body.find("WithIoLock"), std::string::npos);
  const size_t stop = source.find("void ConversationsHub::StopMesh()");
  ASSERT_NE(stop, std::string::npos);
  EXPECT_NE(source.find("StopMeshLinkCountTick();", stop), std::string::npos)
      << "the tick goes before the runtime does";
}

} // namespace
} // namespace pbr
