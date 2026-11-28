#include "foundation/sockets/TcpSockets.h"

namespace pbr::sockets {

bool SendAll(const Socket socket, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const long n = Send(socket, data.data() + sent, data.size() - sent);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool RecvUntil(const Socket socket, std::string& buffer, const std::string& delimiter, const size_t max_bytes) {
  char chunk[4096];
  while (buffer.find(delimiter) == std::string::npos) {
    if (buffer.size() > max_bytes) {
      return false;
    }
    const long n = Recv(socket, chunk, sizeof(chunk));
    if (n <= 0) {
      return false;
    }
    buffer.append(chunk, static_cast<size_t>(n));
  }
  return true;
}

} // namespace pbr::sockets
