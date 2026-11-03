#pragma once

#include "common/ShareableLine.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pbr {

/** One thread's CPU time so far (user + system) in this process. */
struct ThreadCpuSample {
  /** OS thread id (mach port name on Apple); stable while the thread lives. */
  uint32_t id = 0;
  std::string name;
  double cpu_s = 0.0;
};

/** Every thread of this process now (Apple; other platforms: empty). Cheap enough for once every 10 s. */
std::vector<ThreadCpuSample> ReadThreadCpu();

/**
 * Thread names a shareable line may carry (ShareableLine takes no free text). A thread whose name is not
 * here counts as "other".
 */
inline constexpr ShareWord kKnownThreadNames[] = {
    "main",
    "pp.capture",
    "pp.playout",
    "pp.video",
    "pp.devices",
    "pp-media-sess",
    "com.apple.audio.IOThread.client",
    "com.apple.root.user-interactive-qos",
    "com.apple.root.user-initiated-qos",
    "com.apple.root.default-qos",
    "com.apple.root.utility-qos",
    "com.apple.network.connections",
};

/** `name` when it is in kKnownThreadNames, else "other". */
std::string_view CanonicalThreadName(std::string_view name);

struct ThreadCpuDelta {
  std::string name;
  double ms = 0.0;
};

/**
 * CPU used since the previous call, per thread name (CanonicalThreadName; threads of one name are summed),
 * largest first, at most `top_n`, zero entries left out. `previous` is the per-thread-id state between calls:
 * it is replaced by this sample, so threads that ended drop out. The first call (empty `previous`) has no
 * baseline and returns nothing; a thread new since the last call counts all of its CPU.
 */
std::vector<ThreadCpuDelta> TopThreadCpuDeltas(const std::vector<ThreadCpuSample>& now,
                                               std::unordered_map<uint32_t, double>& previous, std::size_t top_n);

/**
 * The same with each thread's own name (only `[A-Za-z0-9._-]`, at most 32 characters; unnamed threads keep
 * their "t<id>"), for the developer log only — never a shareable line, which takes no free text.
 */
std::vector<ThreadCpuDelta> TopThreadCpuDeltasByOwnName(const std::vector<ThreadCpuSample>& now,
                                                        std::unordered_map<uint32_t, double>& previous,
                                                        std::size_t top_n);

} // namespace pbr
