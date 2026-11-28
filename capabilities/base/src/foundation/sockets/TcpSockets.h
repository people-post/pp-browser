#pragma once

#include "common/Error.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

/**
 * Plain TCP sockets (capability-repos CR027: the base's, so nothing needs Asio): listen on a numeric address, accept,
 * connect to loopback, send, receive and poll. Blocking by default; SetNonBlocking for a poll loop. pp-node's status
 * server and the tests' loopback HTTP servers use them. POSIX and Winsock (TcpSockets_Posix.cpp / _Win32.cpp).
 */
namespace pbr::sockets {

/** A socket handle (an fd, or a SOCKET); kInvalid when none. */
using Socket = std::intptr_t;
inline constexpr Socket kInvalid = -1;

struct Listener {
  Socket socket = kInvalid;
  /** The address bound, `host:port` (the port the OS chose when asked for 0). */
  std::string bound;
  uint16_t port = 0;
};

/** Binds `host` (a numeric IPv4 or IPv6 address) and `port` (0: any free one), with SO_REUSEADDR, and listens. */
Roe<Listener> Listen(const std::string& host, uint16_t port, int backlog = 64);
/** Connects to 127.0.0.1:`port` (blocking). */
Roe<Socket> ConnectLoopback(uint16_t port);
/** Waits for (or, non-blocking, takes) the next connection; kInvalid when there is none or on error. */
Socket Accept(Socket listener);

/** Bytes read (> 0), 0 at the peer's end, < 0 on error or, non-blocking, when nothing is there (WouldBlock()). */
long Recv(Socket socket, char* buffer, size_t size);
/** Bytes written (> 0), < 0 on error or, non-blocking, when the socket is full (WouldBlock()). */
long Send(Socket socket, const char* data, size_t size);
/** Writes all of `data` (blocking sockets); false on error. */
bool SendAll(Socket socket, const std::string& data);
/** Reads into `buffer` until it holds `delimiter` (blocking); false on error or the peer's end first. */
bool RecvUntil(Socket socket, std::string& buffer, const std::string& delimiter, size_t max_bytes = 1 << 20);

void ShutdownBoth(Socket socket);
void Close(Socket socket);
bool SetNonBlocking(Socket socket, bool non_blocking);
/** The last call failed only because a non-blocking socket had nothing to do. */
bool WouldBlock();
std::string LastErrorText();

struct PollItem {
  Socket socket = kInvalid;
  bool want_read = false;
  bool want_write = false;
  bool readable = false;
  bool writable = false;
  /** Hung up or failed. */
  bool failed = false;
};
/** Waits up to `timeout_ms` for any item to be ready; how many are, or < 0 on error. */
int Poll(std::vector<PollItem>& items, int timeout_ms);

/** A socket closed when it goes. */
class OwnedSocket {
public:
  OwnedSocket() = default;
  explicit OwnedSocket(Socket socket) : socket_(socket) {}
  ~OwnedSocket() { Reset(); }
  OwnedSocket(OwnedSocket&& other) noexcept : socket_(other.Release()) {}
  OwnedSocket& operator=(OwnedSocket&& other) noexcept {
    if (this != &other) {
      Reset(other.Release());
    }
    return *this;
  }
  OwnedSocket(const OwnedSocket&) = delete;
  OwnedSocket& operator=(const OwnedSocket&) = delete;

  Socket Get() const { return socket_; }
  bool Valid() const { return socket_ != kInvalid; }
  Socket Release() {
    const Socket s = socket_;
    socket_ = kInvalid;
    return s;
  }
  void Reset(Socket socket = kInvalid) {
    if (socket_ != kInvalid) {
      Close(socket_);
    }
    socket_ = socket;
  }

private:
  Socket socket_ = kInvalid;
};

} // namespace pbr::sockets
