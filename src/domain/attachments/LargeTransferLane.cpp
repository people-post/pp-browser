#include "domain/attachments/LargeTransferLane.h"

#include <utility>

namespace pbr {

void LargeTransferLane::Post(const Direction direction, std::function<void()> job) {
  if (!job || Stopping()) {
    return;
  }
  Lane& lane = direction == Direction::Upload ? upload_ : download_;
  std::lock_guard lock(lane.mutex);
  if (Stopping()) {
    return;
  }
  lane.jobs.push_back(std::move(job));
  if (!lane.thread.joinable()) {
    lane.thread = std::thread([this, &lane]() { Run(lane); });
  }
  lane.wake.notify_one();
}

void LargeTransferLane::Run(Lane& lane) {
  while (true) {
    std::function<void()> job;
    {
      std::unique_lock lock(lane.mutex);
      lane.wake.wait(lock, [&]() { return Stopping() || !lane.jobs.empty(); });
      if (Stopping()) {
        return;
      }
      job = std::move(lane.jobs.front());
      lane.jobs.pop_front();
    }
    job();
  }
}

void LargeTransferLane::RequestStop() {
  stopping_.store(true, std::memory_order_release);
  for (Lane* lane : {&upload_, &download_}) {
    std::lock_guard lock(lane->mutex);
    lane->wake.notify_all();
  }
}

void LargeTransferLane::Shutdown() {
  RequestStop();
  for (Lane* lane : {&upload_, &download_}) {
    std::thread thread;
    {
      std::lock_guard lock(lane->mutex);
      thread = std::move(lane->thread);
      lane->jobs.clear();
    }
    if (thread.joinable()) {
      thread.join();
    }
  }
}

} // namespace pbr
