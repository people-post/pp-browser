#pragma once

#include "feature/calls/CallsExecutor.h"
#include "feature/calls/CallsOutbox.h"

#include <chrono>
#include <functional>
#include <memory>

namespace pbr {

/**
 * Tests play the parent: a component's events come straight back to `handle` through the calls
 * owner (posted / delayed like the product queue). Declare it after the component it serves, so it
 * goes first and drops what is still queued.
 */
template <typename Event>
class CallsLoopbackOutbox {
public:
  explicit CallsLoopbackOutbox(std::function<void(Event&)> handle) : handle_(std::move(handle)) {}

  CallsOutbox<Event> Get() {
    typename CallsOutbox<Event>::Sink sink;
    sink.emit = [this](Event event) {
      tasks_.Post([this, event = std::make_shared<Event>(std::move(event))]() { handle_(*event); });
    };
    sink.after = [this](std::chrono::milliseconds delay, Event event) {
      return tasks_.After(delay, [this, event = std::make_shared<Event>(std::move(event))]() { handle_(*event); });
    };
    sink.cancel = [this](CallsExecutor::TimerId id) { tasks_.Cancel(id); };
    return CallsOutbox<Event>(std::move(sink));
  }

private:
  std::function<void(Event&)> handle_;
  CallsTasks tasks_{CallsOwnerExecutor()};
};

} // namespace pbr
