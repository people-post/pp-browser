#include "lib/media/ThreadCpu.h"

#include <mach/mach.h>
#include <pthread.h>

namespace pbr {

std::vector<ThreadCpuSample> ReadThreadCpu() {
  std::vector<ThreadCpuSample> out;
  thread_act_array_t threads = nullptr;
  mach_msg_type_number_t count = 0;
  if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
    return out;
  }
  out.reserve(count);
  for (mach_msg_type_number_t i = 0; i < count; ++i) {
    thread_basic_info_data_t info{};
    mach_msg_type_number_t info_count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(threads[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &info_count) ==
        KERN_SUCCESS) {
      ThreadCpuSample s;
      s.id = static_cast<uint32_t>(threads[i]);
      s.cpu_s = static_cast<double>(info.user_time.seconds + info.system_time.seconds) +
                static_cast<double>(info.user_time.microseconds + info.system_time.microseconds) / 1e6;
      char name[64] = {};
      if (const pthread_t pt = pthread_from_mach_thread_np(threads[i])) {
        pthread_getname_np(pt, name, sizeof(name));
      }
      if (name[0] != '\0') {
        s.name = name;
      } else if (i == 0) {
        s.name = "main"; // task_threads lists the first thread (the process's main thread) first
      } else {
        s.name = "t" + std::to_string(s.id & 0xffff);
      }
      out.push_back(std::move(s));
    }
    mach_port_deallocate(mach_task_self(), threads[i]);
  }
  vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_act_t));
  return out;
}

} // namespace pbr
