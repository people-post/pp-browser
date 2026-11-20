#pragma once

#include "common/runtime/AppRuntime.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <type_traits>
#include <utility>

namespace pbr::test {

/** The app's quit budget (Application::Shutdown). */
inline constexpr std::chrono::milliseconds kTeardownQuiesceBudget{2000};

/**
 * Frees `owners` (in the order given) the way Application frees the hub, the call stack and the rest (THREADING.md
 * § Teardown quiesce): `QuiesceForTeardown` first — what is queued on the UI mailbox, the workers, the coordinator
 * and the owner threads runs while the owners are still alive, then the runtime gate closes — then the frees, then
 * `ReopenAfterTeardown`, so the test goes on (or the next one starts) with an open runtime and a new epoch: a post
 * from before stays dead. Call it on the test's thread (the UI mailbox's): the quiesce pumps that mailbox only there,
 * and before `AppRuntime::Shutdown` when the test started the runtime, as the app does.
 *
 * Owners post raw `this` and leave their lifetime to that gate (no per-owner gates for teardown); a test that frees
 * one without it leaves those posts to run later, in a later test, on freed memory. With it they cannot: they ran
 * before the free, or the gate drops them. Work still pending or running at `budget` fails the test that made it,
 * and the owners are leaked rather than freed under it, as the app leaks messaging when its quit quiesce runs out.
 *
 * @return true when the owners were freed (an empty one is fine).
 */
template <typename... T>
bool FreeLikeTheAppWithin(const std::chrono::milliseconds budget, std::unique_ptr<T>&... owners) {
  if (!(static_cast<bool>(owners) || ...)) {
    return true;
  }
  EXPECT_TRUE(pp::AppRuntime::CurrentlyOnUI()) << "free owners on the UI mailbox's thread: only there is it drained";
  const bool drained = pp::AppRuntime::QuiesceForTeardown(budget);
  EXPECT_TRUE(drained) << "work the owner may touch was still pending or running after " << budget.count()
                       << " ms: the owner is leaked rather than freed under it";
  if (drained) {
    (owners.reset(), ...);
  } else {
    ((void)owners.release(), ...);
  }
  pp::AppRuntime::ReopenAfterTeardown();
  return drained;
}

/** FreeLikeTheAppWithin the app's quit budget. */
template <typename... T>
bool FreeLikeTheApp(std::unique_ptr<T>&... owners) {
  return FreeLikeTheAppWithin(kTeardownQuiesceBudget, owners...);
}

/**
 * A long-lived owner a test holds (a hub, a stack, an outbox): built in place, used through `->` / `*`, and freed
 * like the app frees it (FreeLikeTheApp) when the holder goes or on `Free()`. The quiesce runs its queued posts, so
 * what they reach (the test's fakes, ports, logs) must still be there: declare it after them, so it goes first.
 */
template <typename T>
class TestOwner {
public:
  template <typename... Args>
    requires(!(sizeof...(Args) == 1 && (std::is_same_v<std::remove_cvref_t<Args>, TestOwner> || ...)))
  explicit TestOwner(Args&&... args) : owner_(std::make_unique<T>(std::forward<Args>(args)...)) {}
  ~TestOwner() { Free(); }

  TestOwner(TestOwner&& other) noexcept = default;
  TestOwner& operator=(TestOwner&& other) noexcept {
    if (this != &other) {
      Free();
      owner_ = std::move(other.owner_);
    }
    return *this;
  }
  TestOwner(const TestOwner&) = delete;
  TestOwner& operator=(const TestOwner&) = delete;

  T& operator*() const { return *owner_; }
  T* operator->() const { return owner_.get(); }
  T* get() const { return owner_.get(); }

  /** Frees the owner now (FreeLikeTheApp); the holder is empty afterwards. */
  bool Free() { return FreeLikeTheApp(owner_); }

private:
  std::unique_ptr<T> owner_;
};

} // namespace pbr::test
