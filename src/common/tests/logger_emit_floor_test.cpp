#include "common/Logger.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

using pp::logging::Handler;
using pp::logging::Level;
using pp::logging::Logger;

struct CaptureHandler : Handler {
  void emit(Level level, const std::string& /*loggerName*/,
            const std::string& message) override {
    last_level = level;
    last_message = message;
    ++count;
  }

  Level last_level = Level::DEBUG;
  std::string last_message;
  int count = 0;
};

class EmitFloorGuard {
public:
  EmitFloorGuard() : previous_(pp::logging::getEmitFloor()) {}
  ~EmitFloorGuard() { pp::logging::setEmitFloor(previous_); }

  EmitFloorGuard(const EmitFloorGuard&) = delete;
  EmitFloorGuard& operator=(const EmitFloorGuard&) = delete;

private:
  Level previous_;
};

} // namespace

TEST(LoggerEmitFloorTest, PromotesAfterFilterKeepsOriginalThreshold) {
  EmitFloorGuard restore;
  pp::logging::setEmitFloor(Level::WARNING);

  auto handler = std::make_shared<CaptureHandler>();
  Logger log = pp::logging::getLogger("test.emit_floor.promote");
  log.setLevel(Level::INFO);
  log.setPropagate(false);
  log.addHandler(handler);

  log.info << "visible";
  ASSERT_EQ(handler->count, 1);
  EXPECT_EQ(handler->last_level, Level::WARNING);
  EXPECT_NE(handler->last_message.find("[WARNING]"), std::string::npos);
  EXPECT_NE(handler->last_message.find("visible"), std::string::npos);

  // DEBUG still filtered by logger level (INFO); floor must not bypass that.
  log.debug << "hidden";
  EXPECT_EQ(handler->count, 1);
}

TEST(LoggerEmitFloorTest, DefaultFloorIsNoOp) {
  EmitFloorGuard restore;
  pp::logging::setEmitFloor(pp::logging::kLevelDebug);

  auto handler = std::make_shared<CaptureHandler>();
  Logger log = pp::logging::getLogger("test.emit_floor.noop");
  log.setLevel(Level::INFO);
  log.setPropagate(false);
  log.addHandler(handler);

  log.info << "plain";
  ASSERT_EQ(handler->count, 1);
  EXPECT_EQ(handler->last_level, Level::INFO);
  EXPECT_NE(handler->last_message.find("[INFO]"), std::string::npos);
}
