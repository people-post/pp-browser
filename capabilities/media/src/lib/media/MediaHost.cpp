#include "lib/media/MediaHost.h"

#include <mutex>
#include <utility>

namespace pbr {

namespace {

std::mutex& HostMutex() {
  static std::mutex mu;
  return mu;
}

MediaHost& HostPorts() {
  static MediaHost host;
  return host;
}

} // namespace

void SetMediaHost(MediaHost host) {
  std::lock_guard lock(HostMutex());
  HostPorts() = std::move(host);
}

MediaHost CurrentMediaHost() {
  std::lock_guard lock(HostMutex());
  return HostPorts();
}

} // namespace pbr
