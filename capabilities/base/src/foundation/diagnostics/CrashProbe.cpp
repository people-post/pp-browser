#include "foundation/diagnostics/CrashProbe.h"

#include "foundation/diagnostics/CrashDump.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <thread>

namespace pbr {

std::string CrashProbe::ProbeDir(const std::string& data_dir) {
  return (std::filesystem::path(CrashDump::PendingPath(data_dir)).parent_path() / "probe").string();
}

CrashProbe::Status CrashProbe::ReadStatus(const std::string& data_dir) {
  namespace fs = std::filesystem;
  Status status;
  std::error_code ec;
  std::string newest;
  for (fs::directory_iterator it(ProbeDir(data_dir), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.size() < 5 || name.substr(name.size() - 5) != ".json") {
      continue;
    }
    ++status.payloads;
    if (name > newest) { // names start with the arrival time "YYYYMMDD-HHMMSS"
      newest = name;
    }
  }
  if (newest.size() >= 15) {
    status.last_time = newest.substr(0, 4) + "-" + newest.substr(4, 2) + "-" + newest.substr(6, 2) + " " +
                       newest.substr(9, 2) + ":" + newest.substr(11, 2) + ":" + newest.substr(13, 2);
  }
  return status;
}

void CrashProbe::Crash(const int kind) {
  if (kind == 1) {
    std::thread([] { throw std::runtime_error("probe: uncaught exception"); }).join();
  }
  volatile int* null_pointer = nullptr;
  *null_pointer = 1;
  std::abort();
}

void CrashProbe::Hang(const int seconds) {
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
}

} // namespace pbr
