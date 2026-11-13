#include "foundation/diagnostics/OsCurrentMemory.h"

#include <mach/mach.h>

namespace pbr {

CurrentMemoryMb ReadCurrentMemoryMb() {
  task_vm_info_data_t info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
    return {};
  }
  CurrentMemoryMb out;
  out.footprint_mb = static_cast<int64_t>(info.phys_footprint / 1'000'000ULL);
  out.rss_mb = static_cast<int64_t>(info.resident_size / 1'000'000ULL);
  return out;
}

} // namespace pbr
