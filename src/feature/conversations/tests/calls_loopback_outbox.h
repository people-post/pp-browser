#pragma once

#include "feature/calls/CallsExecutor.h"
#include "foundation/runtime/OwnerOutbox.h"

#include <chrono>
#include <functional>
#include <memory>

namespace pbr {

/**
 * Tests play the parent: a component's events come straight back to `handle` through the calls
 * owner (posted / delayed like the product queue). Outbox copies reach the loopback only through
 * its tasks' shared handle, so a component that outlives the loopback (or cancels in its destructor
 * after it) is harmless: queued events drop and later calls are no-ops.
 */
template <typename Event>
class CallsLoopbackOutbox {
public:
  explicit CallsLoopbackOutbox(std::function<void(Event&)> handle) : handle_(std::move(handle)) {}

  OwnerOutbox<Event> Get() {
    return MakeOwnerOutbox<Event>(tasks_, [this](Event& event) { handle_(event); });
  }

private:
  std::function<void(Event&)> handle_;
  OwnerTasks tasks_{CallsOwnerExecutor()};
};

} // namespace pbr
