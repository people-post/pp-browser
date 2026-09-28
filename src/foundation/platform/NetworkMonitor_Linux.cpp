// Linux desktop: rtnetlink link / address / route groups wake a snapshot; the fingerprint is the
// default-route interfaces and their addresses (bridges / veths of local containers do not count).
// Without netlink, poll every 2 s.

#include "foundation/platform/NetworkMonitorBackend.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>

namespace pbr::detail {
namespace {

std::string ReadFile(const char* path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool IsWireless(const std::string& iface) {
  std::ifstream probe("/sys/class/net/" + iface + "/wireless");
  return probe.good();
}

std::string AddressText(const sockaddr* addr) {
  char buf[INET6_ADDRSTRLEN] = {};
  if (addr->sa_family == AF_INET) {
    const auto* v4 = reinterpret_cast<const sockaddr_in*>(addr);
    const uint32_t host = ntohl(v4->sin_addr.s_addr);
    if ((host >> 16) == 0xa9fe) {  // 169.254/16 link-local
      return {};
    }
    inet_ntop(AF_INET, &v4->sin_addr, buf, sizeof(buf));
  } else if (addr->sa_family == AF_INET6) {
    const auto* v6 = reinterpret_cast<const sockaddr_in6*>(addr);
    if (IN6_IS_ADDR_LINKLOCAL(&v6->sin6_addr)) {
      return {};
    }
    inet_ntop(AF_INET6, &v6->sin6_addr, buf, sizeof(buf));
  }
  return buf;
}

NetworkState Snapshot() {
  std::vector<std::string> ifaces = ParseProcNetRouteDefaultIfaces(ReadFile("/proc/net/route"));
  const auto v6 = ParseProcNetIpv6RouteDefaultIfaces(ReadFile("/proc/net/ipv6_route"));
  ifaces.insert(ifaces.end(), v6.begin(), v6.end());

  NetworkState state;
  std::vector<std::string> parts;
  bool wireless = false;
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) == 0) {
    for (const ifaddrs* it = list; it != nullptr; it = it->ifa_next) {
      if (it->ifa_addr == nullptr || it->ifa_name == nullptr || (it->ifa_flags & IFF_UP) == 0) {
        continue;
      }
      const std::string name = it->ifa_name;
      if (std::find(ifaces.begin(), ifaces.end(), name) == ifaces.end()) {
        continue;
      }
      const std::string addr = AddressText(it->ifa_addr);
      if (!addr.empty()) {
        parts.push_back(name + "/" + addr);
        wireless = wireless || IsWireless(name);
      }
    }
    freeifaddrs(list);
  }
  state.online = !parts.empty();
  state.transport = !state.online ? NetworkTransport::Unknown : wireless ? NetworkTransport::Wifi : NetworkTransport::Other;
  state.fingerprint = JoinSorted(std::move(parts));
  return state;
}

int OpenRouteSocket() {
  const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
  if (fd < 0) {
    return -1;
  }
  sockaddr_nl addr{};
  addr.nl_family = AF_NETLINK;
  addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE;
  if (bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

void Drain(const int fd) {
  char buf[8192];
  while (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0) {
  }
}

class LinuxNetworkMonitorBackend final : public INetworkMonitorBackend {
public:
  ~LinuxNetworkMonitorBackend() override { Stop(); }

  bool Start(StateSink sink) override {
    if (thread_.joinable()) {
      return true;
    }
    if (pipe2(wake_, O_CLOEXEC) != 0) {
      return false;
    }
    sink_ = std::move(sink);
    route_fd_ = OpenRouteSocket();
    thread_ = std::thread([this] { Run(); });
    return true;
  }

  void Stop() override {
    if (!thread_.joinable()) {
      return;
    }
    const char byte = 0;
    (void)!write(wake_[1], &byte, 1);
    thread_.join();
    for (int* fd : {&wake_[0], &wake_[1], &route_fd_}) {
      if (*fd >= 0) {
        close(*fd);
        *fd = -1;
      }
    }
  }

private:
  void Run() {
    sink_(Snapshot());
    constexpr int kPollFallbackMs = 2000;
    constexpr int kSettleMs = 200;  // one change arrives as a burst of netlink messages
    for (;;) {
      pollfd fds[2] = {{wake_[0], POLLIN, 0}, {route_fd_, POLLIN, 0}};
      const int n = poll(fds, route_fd_ >= 0 ? 2 : 1, route_fd_ >= 0 ? -1 : kPollFallbackMs);
      if (n < 0 && errno != EINTR) {
        return;
      }
      if ((fds[0].revents & POLLIN) != 0) {
        return;
      }
      if (route_fd_ >= 0 && (fds[1].revents & POLLIN) == 0) {
        continue;
      }
      if (route_fd_ >= 0) {
        do {
          Drain(route_fd_);
        } while (poll(&fds[1], 1, kSettleMs) > 0 && (fds[1].revents & POLLIN) != 0);
      }
      sink_(Snapshot());
    }
  }

  StateSink sink_;
  std::thread thread_;
  int wake_[2] = {-1, -1};
  int route_fd_ = -1;
};

} // namespace

std::unique_ptr<INetworkMonitorBackend> CreateNetworkMonitorBackend() {
  return std::make_unique<LinuxNetworkMonitorBackend>();
}

} // namespace pbr::detail
