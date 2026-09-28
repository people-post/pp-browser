#include "foundation/platform/NetworkMonitor.h"

#include "foundation/platform/NetworkMonitorBackend.h"

#include <algorithm>
#include <sstream>

namespace pbr {

namespace detail {

namespace {

std::vector<std::string> SplitFields(std::string_view line) {
  std::vector<std::string> fields;
  std::istringstream in{std::string(line)};
  std::string field;
  while (in >> field) {
    fields.push_back(field);
  }
  return fields;
}

template <typename OnLine>
void ForEachLine(std::string_view text, OnLine on_line) {
  size_t start = 0;
  while (start < text.size()) {
    const size_t end = text.find('\n', start);
    on_line(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
}

} // namespace

std::vector<std::string> ParseProcNetRouteDefaultIfaces(std::string_view text) {
  // Iface Destination Gateway Flags RefCnt Use Metric Mask ... (hex); header line first.
  constexpr unsigned kRouteUp = 0x1;
  std::vector<std::string> ifaces;
  ForEachLine(text, [&](std::string_view line) {
    const auto f = SplitFields(line);
    if (f.size() < 8 || f[0] == "Iface") {
      return;
    }
    unsigned flags = 0;
    try {
      flags = static_cast<unsigned>(std::stoul(f[3], nullptr, 16));
    } catch (...) {
      return;
    }
    if (f[1] == "00000000" && f[7] == "00000000" && (flags & kRouteUp) != 0) {
      ifaces.push_back(f[0]);
    }
  });
  return ifaces;
}

std::vector<std::string> ParseProcNetIpv6RouteDefaultIfaces(std::string_view text) {
  // dest(32 hex) dest_len src src_len next_hop metric refcnt use flags iface
  constexpr unsigned kRouteUp = 0x1;
  const std::string any(32, '0');
  std::vector<std::string> ifaces;
  ForEachLine(text, [&](std::string_view line) {
    const auto f = SplitFields(line);
    if (f.size() < 10 || f[0] != any || f[1] != "00" || f[9] == "lo") {
      return;
    }
    unsigned flags = 0;
    try {
      flags = static_cast<unsigned>(std::stoul(f[8], nullptr, 16));
    } catch (...) {
      return;
    }
    if ((flags & kRouteUp) != 0) {
      ifaces.push_back(f[9]);
    }
  });
  return ifaces;
}

std::string JoinSorted(std::vector<std::string> parts) {
  std::sort(parts.begin(), parts.end());
  parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
  std::string out;
  for (const auto& part : parts) {
    if (!out.empty()) {
      out += ',';
    }
    out += part;
  }
  return out;
}

} // namespace detail

NetworkMonitor::NetworkMonitor(const bool with_os_backend)
    : backend_(with_os_backend ? detail::CreateNetworkMonitorBackend() : nullptr) {}

NetworkMonitor::~NetworkMonitor() { Stop(); }

bool NetworkMonitor::Start(Listener listener) {
  {
    std::lock_guard lock(listener_mu_);
    listener_ = std::move(listener);
  }
  {
    std::lock_guard lock(state_mu_);
    if (running_) {
      return true;
    }
    running_ = true;
  }
  if (!backend_) {
    return false;
  }
  return backend_->Start([this](const NetworkState& state) { OnState(state); });
}

void NetworkMonitor::Stop() {
  {
    std::lock_guard lock(state_mu_);
    if (!running_) {
      return;
    }
    running_ = false;
  }
  if (backend_) {
    backend_->Stop();
  }
  std::lock_guard lock(listener_mu_);  // waits out a listener call in flight
  listener_ = nullptr;
}

NetworkState NetworkMonitor::Current() const {
  std::lock_guard lock(state_mu_);
  return state_;
}

uint64_t NetworkMonitor::Generation() const {
  std::lock_guard lock(state_mu_);
  return generation_;
}

void NetworkMonitor::OnState(const NetworkState& state) {
  // Serialized end to end: backends with several OS sources (Windows) may call concurrently, and
  // changes must reach the listener in the order they were judged.
  std::lock_guard serial(listener_mu_);
  NetworkChange change;
  {
    std::lock_guard lock(state_mu_);
    if (!running_) {
      return;
    }
    if (!have_baseline_) {
      have_baseline_ = true;
      state_ = state;
      change.current = state;  // generation 0: the baseline
    } else if (state == state_) {
      return;
    } else {
      change.generation = ++generation_;
      change.previous = state_;
      change.current = state;
      state_ = state;
    }
  }
  if (listener_) {
    listener_(change);
  }
}

} // namespace pbr
