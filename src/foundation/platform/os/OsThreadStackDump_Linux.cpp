#include "foundation/platform/os/OsThreadStackDump.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

// Android Bionic has no execinfo/backtrace (still defines __linux__).
#if defined(__linux__) && !defined(__ANDROID__)
#include <csignal>
#include <cstdio>
#include <execinfo.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace pbr::os {

#if defined(__linux__) && !defined(__ANDROID__)
namespace {

std::atomic_flag g_stack_dump_busy = ATOMIC_FLAG_INIT;

void DumpThisThreadStack(int /*sig*/) {
  while (g_stack_dump_busy.test_and_set(std::memory_order_acquire)) {
  }
  void* frames[64];
  const int n = backtrace(frames, 64);
  char header[64];
  const int len =
      std::snprintf(header, sizeof(header), "--- thread %ld\n", static_cast<long>(syscall(SYS_gettid)));
  (void)!write(STDERR_FILENO, header, static_cast<size_t>(len));
  backtrace_symbols_fd(frames, n, STDERR_FILENO);
  g_stack_dump_busy.clear(std::memory_order_release);
}

} // namespace

void DumpAllThreadStacks() {
  void* warm[1];
  (void)backtrace(warm, 1); // load libgcc unwinder outside the signal handler
  std::signal(SIGUSR2, DumpThisThreadStack);
  const long self = static_cast<long>(syscall(SYS_gettid));
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", ec)) {
    const long tid = std::strtol(entry.path().filename().c_str(), nullptr, 10);
    if (tid > 0 && tid != self) {
      syscall(SYS_tgkill, static_cast<long>(getpid()), tid, SIGUSR2);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

#else

void DumpAllThreadStacks() {}

#endif

} // namespace pbr::os
