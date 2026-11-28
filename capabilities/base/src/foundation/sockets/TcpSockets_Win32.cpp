#if defined(_WIN32)

#include "foundation/sockets/TcpSockets.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <mutex>
#include <string>

namespace pbr::sockets {
namespace {

void EnsureStarted() {
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data;
    (void)WSAStartup(MAKEWORD(2, 2), &data);
  });
}

SOCKET Native(const Socket socket) {
  return static_cast<SOCKET>(socket);
}

Socket Wrap(const SOCKET socket) {
  return socket == INVALID_SOCKET ? kInvalid : static_cast<Socket>(socket);
}

std::string AddressText(const sockaddr_storage& address, uint16_t& port) {
  char text[INET6_ADDRSTRLEN] = {};
  if (address.ss_family == AF_INET6) {
    const auto& a = reinterpret_cast<const sockaddr_in6&>(address);
    inet_ntop(AF_INET6, &a.sin6_addr, text, sizeof(text));
    port = ntohs(a.sin6_port);
  } else {
    const auto& a = reinterpret_cast<const sockaddr_in&>(address);
    inet_ntop(AF_INET, &a.sin_addr, text, sizeof(text));
    port = ntohs(a.sin_port);
  }
  return text;
}

} // namespace

Roe<Listener> Listen(const std::string& host, const uint16_t port, const int backlog) {
  EnsureStarted();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;
  addrinfo* found = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found) {
    return Error("not a numeric address: " + host);
  }
  const SOCKET s = ::socket(found->ai_family, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    freeaddrinfo(found);
    return Error("socket: " + LastErrorText());
  }
  const BOOL yes = TRUE;
  ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
  if (::bind(s, found->ai_addr, static_cast<int>(found->ai_addrlen)) != 0) {
    const std::string why = LastErrorText();
    freeaddrinfo(found);
    ::closesocket(s);
    return Error("bind " + host + ":" + std::to_string(port) + ": " + why);
  }
  freeaddrinfo(found);
  if (::listen(s, backlog) != 0) {
    const std::string why = LastErrorText();
    ::closesocket(s);
    return Error("listen: " + why);
  }
  Listener listener;
  listener.socket = Wrap(s);
  sockaddr_storage local{};
  int length = sizeof(local);
  if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &length) == 0) {
    listener.bound = AddressText(local, listener.port);
    listener.bound += ":" + std::to_string(listener.port);
  } else {
    listener.port = port;
    listener.bound = host + ":" + std::to_string(port);
  }
  return listener;
}

Roe<Socket> ConnectLoopback(const uint16_t port) {
  EnsureStarted();
  const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    return Error("socket: " + LastErrorText());
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    const std::string why = LastErrorText();
    ::closesocket(s);
    return Error("connect 127.0.0.1:" + std::to_string(port) + ": " + why);
  }
  return Wrap(s);
}

Socket Accept(const Socket listener) {
  return Wrap(::accept(Native(listener), nullptr, nullptr));
}

long Recv(const Socket socket, char* buffer, const size_t size) {
  return ::recv(Native(socket), buffer, static_cast<int>(size), 0);
}

long Send(const Socket socket, const char* data, const size_t size) {
  return ::send(Native(socket), data, static_cast<int>(size), 0);
}

void ShutdownBoth(const Socket socket) {
  if (socket != kInvalid) {
    ::shutdown(Native(socket), SD_BOTH);
  }
}

void Close(const Socket socket) {
  if (socket != kInvalid) {
    ::closesocket(Native(socket));
  }
}

bool SetNonBlocking(const Socket socket, const bool non_blocking) {
  u_long mode = non_blocking ? 1 : 0;
  return ::ioctlsocket(Native(socket), FIONBIO, &mode) == 0;
}

bool WouldBlock() {
  return WSAGetLastError() == WSAEWOULDBLOCK;
}

std::string LastErrorText() {
  return "winsock error " + std::to_string(WSAGetLastError());
}

int Poll(std::vector<PollItem>& items, const int timeout_ms) {
  std::vector<WSAPOLLFD> fds(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    fds[i].fd = Native(items[i].socket);
    fds[i].events = static_cast<short>((items[i].want_read ? POLLRDNORM : 0) | (items[i].want_write ? POLLWRNORM : 0));
  }
  const int ready = ::WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeout_ms);
  for (size_t i = 0; i < items.size(); ++i) {
    items[i].readable = (fds[i].revents & POLLRDNORM) != 0;
    items[i].writable = (fds[i].revents & POLLWRNORM) != 0;
    items[i].failed = (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
  }
  return ready;
}

} // namespace pbr::sockets

#endif
