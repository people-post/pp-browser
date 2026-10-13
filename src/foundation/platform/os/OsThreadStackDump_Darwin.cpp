#if defined(__APPLE__)

#include "foundation/platform/os/OsThreadStackDump.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <execinfo.h>
#include <mach/mach.h>
#include <pthread.h>
#include <thread>
#include <unistd.h>

namespace pbr::os {

namespace {

std::atomic_flag g_stack_dump_busy = ATOMIC_FLAG_INIT;

// Runs inside each signalled thread: only async-signal-safe calls (backtrace, write).
void DumpThisThreadStack(int /*sig*/) {
  while (g_stack_dump_busy.test_and_set(std::memory_order_acquire)) {
  }
  void* frames[64];
  const int n = backtrace(frames, 64);
  uint64_t tid = 0;
  pthread_threadid_np(nullptr, &tid);
  char header[96];
  const int len = std::snprintf(header, sizeof(header), "--- thread %llu%s\n", static_cast<unsigned long long>(tid),
                                pthread_main_np() ? " (main)" : "");
  (void)!write(STDERR_FILENO, header, static_cast<size_t>(len));
  backtrace_symbols_fd(frames, n, STDERR_FILENO);
  g_stack_dump_busy.clear(std::memory_order_release);
}

} // namespace

// Every thread of the process gets SIGUSR2 and prints its own backtrace to stderr (the console log the
// device launch script captures). Used by the first-present watchdog (B79) and lab probes.
void DumpAllThreadStacks() {
  void* warm[1];
  (void)backtrace(warm, 1); // load the unwinder outside the signal handler
  std::signal(SIGUSR2, DumpThisThreadStack);

  thread_act_array_t threads = nullptr;
  mach_msg_type_number_t count = 0;
  if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
    return;
  }
  const mach_port_t self = mach_thread_self();
  for (mach_msg_type_number_t i = 0; i < count; ++i) {
    if (threads[i] != self) {
      if (const pthread_t pt = pthread_from_mach_thread_np(threads[i])) {
        pthread_kill(pt, SIGUSR2);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
    mach_port_deallocate(mach_task_self(), threads[i]);
  }
  mach_port_deallocate(mach_task_self(), self);
  vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_act_t));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

} // namespace pbr::os

#endif
