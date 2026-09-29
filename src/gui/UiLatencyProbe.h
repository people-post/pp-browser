#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace pbr {

/**
 * Measures how long the UI thread takes to run a posted task — the real "UI is stuck" time. Frame
 * gaps can't tell: the main loop sleeps while idle, so a quiet call screen has multi-second gaps
 * (metrics device test 2026-09-29). Every kIntervalMs a probe thread posts one ping (never more than
 * one in flight); on_latency runs on the UI thread with the ping's delivery delay. Start / Stop from
 * the UI thread; Stop joins.
 */
class UiLatencyProbe {
public:
  static constexpr int64_t kIntervalMs = 500;
  using PostFn = std::function<void(std::function<void()>)>;
  using LatencyFn = std::function<void(int64_t latency_ms)>;

  UiLatencyProbe() = default;
  UiLatencyProbe(const UiLatencyProbe&) = delete;
  UiLatencyProbe& operator=(const UiLatencyProbe&) = delete;
  ~UiLatencyProbe() { Stop(); }

  bool Running() const { return thread_.joinable(); }

  void Start(PostFn post, LatencyFn on_latency) {
    if (thread_.joinable()) {
      return;
    }
    state_ = std::make_shared<State>();
    state_->on_latency = std::move(on_latency);
    thread_ = std::thread([state = state_, post = std::move(post)]() {
      std::unique_lock lock(state->mu);
      while (!state->stop) {
        state->cv.wait_for(lock, std::chrono::milliseconds(kIntervalMs), [&] { return state->stop; });
        if (state->stop || state->in_flight.load()) {
          continue;  // a ping still waiting: its latency keeps growing until the UI runs it
        }
        state->in_flight = true;
        const auto sent = std::chrono::steady_clock::now();
        lock.unlock();
        post([state, sent]() {
          const auto latency =
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sent).count();
          state->in_flight = false;
          if (!state->stopped_ui && state->on_latency) {
            state->on_latency(latency);
          }
        });
        lock.lock();
      }
    });
  }

  void Stop() {
    if (!thread_.joinable()) {
      return;
    }
    {
      std::lock_guard lock(state_->mu);
      state_->stop = true;
    }
    state_->stopped_ui = true;  // a ping still queued on the UI thread must not report
    state_->cv.notify_all();
    thread_.join();
    state_.reset();
  }

private:
  struct State {
    std::mutex mu;
    std::condition_variable cv;
    bool stop = false;
    std::atomic<bool> in_flight{false};
    std::atomic<bool> stopped_ui{false};
    LatencyFn on_latency;
  };

  std::shared_ptr<State> state_;
  std::thread thread_;
};

} // namespace pbr
