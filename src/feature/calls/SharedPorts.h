#pragma once

#include <memory>
#include <mutex>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * A ports struct that is swapped whole and read as an immutable snapshot (projects/thread-ownership
 * rule 2). Readers take one `Get()` per operation and use only that snapshot — a check and the call
 * it guards must see the same ports, even if `Set` runs meanwhile on another thread. Replaces
 * rebinding a live struct under callers (the port-rebind race: a running std::function destroyed
 * by its own rebind).
 */
template <typename Ports>
class SharedPorts {
public:
  SharedPorts() : ports_(std::make_shared<const Ports>()) {}

  void Set(Ports ports) {
    auto next = std::make_shared<const Ports>(std::move(ports));
    std::lock_guard lock(mu_);
    ports_.swap(next);
    // `next` (the old snapshot) is released outside the lock; in-flight readers keep theirs alive.
  }

  std::shared_ptr<const Ports> Get() const {
    std::lock_guard lock(mu_);
    return ports_;
  }

private:
  mutable std::mutex mu_;
  std::shared_ptr<const Ports> ports_;
};

} // namespace pbr
