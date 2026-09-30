#include "feature/calls/CallsLoop.h"

#include <gtest/gtest.h>

#include <deque>
#include <map>
#include <string>
#include <vector>

namespace pbr {
namespace {

/** The calls owner by hand: tasks run when the test drains, timers when it fires them. */
class ManualExecutor final : public CallsExecutor {
public:
  void Post(std::function<void()> task) override { queue.push_back(std::move(task)); }
  void PostFront(std::function<void()> task) override { queue.push_front(std::move(task)); }
  TimerId After(std::chrono::milliseconds delay, std::function<void()> task) override {
    const TimerId id = ++next;
    timers[id] = {delay, std::move(task)};
    return id;
  }
  void Cancel(TimerId id) override { timers.erase(id); }
  bool IsCurrent() const override { return true; }

  void Drain() {
    while (!queue.empty()) {
      auto task = std::move(queue.front());
      queue.pop_front();
      task();
    }
  }
  void Fire(TimerId id) {
    auto it = timers.find(id);
    ASSERT_NE(it, timers.end());
    auto task = std::move(it->second.second);
    timers.erase(it);
    task();
  }

  std::deque<std::function<void()>> queue;
  std::map<TimerId, std::pair<std::chrono::milliseconds, std::function<void()>>> timers;
  TimerId next = 0;
};

std::string Name(const CallStackEvent& event) { return CallStackEventName(event); }

// Events run in order on the owner; one enqueued while another is handled runs after it.
TEST(CallsLoopTest, EventsRunInOrderAndNeverReenter) {
  ManualExecutor executor;
  std::vector<std::string> handled;
  bool in_handler = false;
  CallsLoop* self = nullptr;
  CallsLoop loop(executor, [&](CallStackEvent& event) {
    EXPECT_FALSE(in_handler) << "re-entered";
    in_handler = true;
    handled.push_back(Name(event));
    if (std::holds_alternative<calls_event::RelayChosen>(event)) {
      self->Enqueue(calls_event::ObservedAddressChanged{});
      EXPECT_EQ(handled.back(), "RelayChosen") << "not handled inline";
    }
    in_handler = false;
  });
  self = &loop;
  loop.Enqueue(calls_event::RelayChosen{"12D3r1"});
  loop.Enqueue(calls_event::MobilityOverrideChanged{});
  loop.EnqueueFront(calls_event::MobilityWake{});
  executor.Drain();
  EXPECT_EQ(handled, (std::vector<std::string>{"MobilityWake", "RelayChosen", "MobilityOverrideChanged",
                                               "ObservedAddressChanged"}));
}

TEST(CallsLoopTest, DelayedEventsCancelAndDropWithTheLoop) {
  ManualExecutor executor;
  std::vector<std::string> handled;
  {
    CallsLoop loop(executor, [&](CallStackEvent& event) { handled.push_back(Name(event)); });
    auto kept = loop.After(std::chrono::milliseconds(50), calls_event::MobilityWake{});
    auto cancelled = loop.After(std::chrono::milliseconds(50), calls_event::ObservedAddressChanged{});
    const auto cancelled_id = cancelled;
    loop.Cancel(cancelled);
    EXPECT_EQ(cancelled, 0u);
    EXPECT_EQ(executor.timers.count(cancelled_id), 0u);
    executor.Fire(kept);
    EXPECT_EQ(handled, (std::vector<std::string>{"MobilityWake"}));

    loop.Enqueue(calls_event::MobilityOverrideChanged{});
    loop.After(std::chrono::milliseconds(50), calls_event::MobilityWake{});
  }
  EXPECT_TRUE(executor.timers.empty()) << "the loop's timers go with it";
  executor.Drain();
  EXPECT_EQ(handled.size(), 1u) << "an event queued for a gone loop is dropped";
}

TEST(CallsLoopTest, WakeSlotArmsOnceAtADeadline) {
  ManualExecutor executor;
  int wakes = 0;
  CallsLoop loop(executor, [&](CallStackEvent&) { ++wakes; });
  CallsWakeSlot slot(loop, calls_event::MobilityWake{});
  const auto at = CallsWakeSlot::Clock::now() + std::chrono::minutes(5);
  slot.ArmAt(at);
  slot.ArmAt(at);
  EXPECT_EQ(executor.timers.size(), 1u) << "same deadline: same timer";
  slot.ArmAt(at + std::chrono::minutes(1));
  ASSERT_EQ(executor.timers.size(), 1u) << "moved, not added";
  EXPECT_GE(executor.timers.begin()->second.first, std::chrono::minutes(5));
  slot.ArmAt(std::nullopt);
  EXPECT_TRUE(executor.timers.empty());

  slot.ArmAt(at);
  executor.Fire(executor.timers.begin()->first);
  EXPECT_EQ(wakes, 1);
  slot.Fired();
  slot.ArmAt(at);
  EXPECT_EQ(executor.timers.size(), 1u) << "after the wake the same deadline arms again";
}

} // namespace
} // namespace pbr
