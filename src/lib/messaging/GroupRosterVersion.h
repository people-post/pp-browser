#pragma once

#include <atomic>
#include <cstdint>

namespace pbr {

/**
 * A counter that moves whenever a group's membership may have changed on this device (a membership event applied
 * from a peer, or one this device wrote). Cheap to read from any thread: the composer's `@` list compares it
 * with the one its lines were read at, instead of re-reading the roster to find out.
 */
class GroupRosterVersion {
public:
  static void Bump() { Counter().fetch_add(1, std::memory_order_relaxed); }
  static uint64_t Get() { return Counter().load(std::memory_order_relaxed); }

private:
  static std::atomic<uint64_t>& Counter() {
    static std::atomic<uint64_t> counter{0};
    return counter;
  }
};

} // namespace pbr
