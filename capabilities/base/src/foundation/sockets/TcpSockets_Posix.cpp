#if !defined(_WIN32)

#include "foundation/sockets/TcpSockets.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace pbr::sockets {
namespace {

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

int Fd(const Socket socket) {
  return static_cast<int>(socket);
}

} // namespace

Roe<Listener> Listen(const std::string& host, const uint16_t port, const int backlog) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;
  addrinfo* found = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found) {
    return Error("not a numeric address: " + host);
  }
  const int fd = ::socket(found->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    freeaddrinfo(found);
    return Error("socket: " + LastErrorText());
  }
  const int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  if (::bind(fd, found->ai_addr, found->ai_addrlen) != 0) {
    const std::string why = LastErrorText();
    freeaddrinfo(found);
    ::close(fd);
    return Error("bind " + host + ":" + std::to_string(port) + ": " + why);
  }
  freeaddrinfo(found);
  if (::listen(fd, backlog) != 0) {
    const std::string why = LastErrorText();
    ::close(fd);
    return Error("listen: " + why);
  }
  Listener listener;
  listener.socket = fd;
  sockaddr_storage local{};
  socklen_t length = sizeof(local);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &length) == 0) {
    listener.bound = AddressText(local, listener.port);
    listener.bound += ":" + std::to_string(listener.port);
  } else {
    listener.port = port;
    listener.bound = host + ":" + std::to_string(port);
  }
  return listener;
}

Roe<Socket> ConnectLoopback(const uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return Error("socket: " + LastErrorText());
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    const std::string why = LastErrorText();
    ::close(fd);
    return Error("connect 127.0.0.1:" + std::to_string(port) + ": " + why);
  }
  return static_cast<Socket>(fd);
}

Socket Accept(const Socket listener) {
  const int fd = ::accept(Fd(listener), nullptr, nullptr);
  if (fd < 0) {
    return kInvalid;
  }
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fd;
}

long Recv(const Socket socket, char* buffer, const size_t size) {
  ssize_t n;
  do {
    n = ::recv(Fd(socket), buffer, size, 0);
  } while (n < 0 && errno == EINTR);
  return static_cast<long>(n);
}

long Send(const Socket socket, const char* data, const size_t size) {
#if defined(MSG_NOSIGNAL)
  constexpr int kFlags = MSG_NOSIGNAL;
#else
  constexpr int kFlags = 0;
#endif
  ssize_t n;
  do {
    n = ::send(Fd(socket), data, size, kFlags);
  } while (n < 0 && errno == EINTR);
  return static_cast<long>(n);
}

void ShutdownBoth(const Socket socket) {
  if (socket != kInvalid) {
    ::shutdown(Fd(socket), SHUT_RDWR);
  }
}

void Close(const Socket socket) {
  if (socket != kInvalid) {
    ::close(Fd(socket));
  }
}

bool SetNonBlocking(const Socket socket, const bool non_blocking) {
  const int flags = ::fcntl(Fd(socket), F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(Fd(socket), F_SETFL, non_blocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
}

bool WouldBlock() {
  return errno == EAGAIN || errno == EWOULDBLOCK;
}

std::string LastErrorText() {
  return std::strerror(errno);
}

int Poll(std::vector<PollItem>& items, const int timeout_ms) {
  std::vector<pollfd> fds(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    fds[i].fd = Fd(items[i].socket);
    fds[i].events = static_cast<short>((items[i].want_read ? POLLIN : 0) | (items[i].want_write ? POLLOUT : 0));
  }
  int ready;
  do {
    ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout_ms);
  } while (ready < 0 && errno == EINTR);
  for (size_t i = 0; i < items.size(); ++i) {
    items[i].readable = (fds[i].revents & POLLIN) != 0;
    items[i].writable = (fds[i].revents & POLLOUT) != 0;
    items[i].failed = (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
  }
  return ready;
}

} // namespace pbr::sockets

#endif
