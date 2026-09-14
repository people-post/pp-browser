#include "feature/calls/CallMediaSeat.h"

#include "foundation/runtime/AppRuntime.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace pbr {
namespace {

TEST(CallMediaSeatTest, AcquireReleasesPriorBind) {
  AppRuntime::Initialize();
  AppRuntime::InitializeUI();

  CallMediaSeat seat;
  std::vector<std::string> topo_stopped;
  std::vector<std::string> engine_stopped;
  seat.SetTeardownHooks(
      [&](const std::string& id) { topo_stopped.push_back(id); },
      [&](const std::string& id, uint64_t /*epoch*/, bool /*force*/) {
        engine_stopped.push_back(id);
      });

  auto a = seat.Acquire("call:a");
  EXPECT_EQ(a.call_id, "call:a");
  EXPECT_TRUE(seat.IsBound("call:a"));

  auto b = seat.Acquire("call:b");
  AppRuntime::RunUITasks();
  EXPECT_EQ(b.call_id, "call:b");
  EXPECT_TRUE(seat.IsBound("call:b"));
  EXPECT_FALSE(seat.IsBound("call:a"));
  ASSERT_FALSE(topo_stopped.empty());
  EXPECT_EQ(topo_stopped.back(), "call:a");
  ASSERT_FALSE(engine_stopped.empty());
  EXPECT_EQ(engine_stopped.back(), "call:a");

  AppRuntime::Shutdown();
  AppRuntime::ShutdownUI();
}

TEST(CallMediaSeatTest, StaleReleaseNoOpAfterNoteStart) {
  // Dogfood b60d82: Accept leftover Stop must not kill new StartSfu.
  AppRuntime::Initialize();
  AppRuntime::InitializeUI();

  CallMediaSeat seat;
  int engine_stops = 0;
  seat.SetTeardownHooks({}, [&](const std::string& /*id*/, uint64_t epoch_at_post, bool force) {
    if (!force && seat.Epoch() != epoch_at_post) {
      return;
    }
    ++engine_stops;
  });

  seat.Acquire("call:old");
  const uint64_t epoch_before_release = seat.Epoch();
  seat.Release("call:old");
  // Simulate Accept scheduling Stop, then new call StartSfu + NoteStart before Stop runs.
  seat.Acquire("call:new");
  seat.NoteStart("call:new");
  EXPECT_NE(seat.Epoch(), epoch_before_release);
  EXPECT_TRUE(seat.IsBound("call:new"));

  AppRuntime::RunUITasks();
  // Force prior Acquire teardown may have stopped old; the Release stop for old must no-op
  // after NoteStart (epoch advanced). Net: at most the force stop from Acquire(new), not a
  // second kill of call:new's session.
  EXPECT_TRUE(seat.IsBound("call:new"));
  EXPECT_GE(engine_stops, 1);

  // Explicit stale token Release must no-op.
  CallMediaSeat::Token stale;
  stale.call_id = "call:old";
  stale.epoch = epoch_before_release;
  const int stops_before = engine_stops;
  seat.Release(stale);
  AppRuntime::RunUITasks();
  EXPECT_EQ(engine_stops, stops_before);
  EXPECT_TRUE(seat.IsBound("call:new"));

  AppRuntime::Shutdown();
  AppRuntime::ShutdownUI();
}

TEST(CallMediaSeatTest, NotePathDoesNotRelease) {
  CallMediaSeat seat;
  int stops = 0;
  seat.SetTeardownHooks({}, [&](const std::string&, uint64_t, bool) { ++stops; });
  seat.Acquire("call:1");
  seat.NoteStart("call:1");
  seat.NotePath(CallMediaSeat::PathKind::Hop);
  EXPECT_EQ(seat.Path(), CallMediaSeat::PathKind::Hop);
  EXPECT_TRUE(seat.IsBound("call:1"));
  EXPECT_EQ(stops, 0);
}

} // namespace
} // namespace pbr
