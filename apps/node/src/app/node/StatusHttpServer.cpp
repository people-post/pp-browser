#include "app/node/StatusHttpServer.h"

#include "common/Logger.h"
#include "foundation/sockets/TcpSockets.h"

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr size_t kMaxRequestBytes = 16 * 1024;
// PB-027: a client that opens connections and sends nothing (or one byte a minute) cannot hold the
// node's file descriptors: each request must arrive within the deadline, and only so many are open.
constexpr std::chrono::seconds kRequestDeadline{5};
constexpr int kMaxOpenConnections = 64;
// How often the loop looks at deadlines and Stop when nothing happens.
constexpr int kPollMs = 100;

using Clock = std::chrono::steady_clock;

struct Connection {
  sockets::OwnedSocket socket;
  Clock::time_point deadline;
  std::string in;
  std::string out;
  size_t sent = 0;
  bool answering = false;
};

} // namespace

struct StatusHttpServer::Impl {
  sockets::OwnedSocket listener;
  StatusHttpAuthConfig auth;
  SnapshotFn snapshot;
  StatusHttpActions actions;
  std::string bound;
  std::atomic<bool> stopping{false};

  /** A complete request answered: what to write back. */
  std::string Answer(const StatusHttpRequest& request) {
    StatusHttpSnapshot snap;
    try {
      snap = snapshot ? snapshot() : StatusHttpSnapshot{};
    } catch (...) {
      snap = StatusHttpSnapshot{};
    }
    StatusHttpResponse response;
    try {
      response = HandleStatusHttpRequest(request, auth, snap, actions);
    } catch (...) {
      response = StatusHttpInternalError(); // a request never stops the server
    }
    if (request.method == "HEAD") {
      response.body.clear();
    }
    return FormatStatusHttpResponse(response);
  }

  /** Reads what is there; false when the connection is done (closed, too big, failed). */
  bool ReadFrom(Connection& c) {
    char chunk[1024];
    while (true) {
      const long n = sockets::Recv(c.socket.Get(), chunk, sizeof(chunk));
      if (n == 0) {
        return false;
      }
      if (n < 0) {
        return sockets::WouldBlock();
      }
      c.in.append(chunk, static_cast<size_t>(n));
      if (c.in.size() > kMaxRequestBytes) {
        return false;
      }
      if (auto request = TryParseStatusHttpRequest(c.in)) {
        c.out = Answer(*request);
        c.answering = true;
        return true;
      }
    }
  }

  /** Writes what it can; false when the answer is out (or the write failed). */
  bool WriteTo(Connection& c) {
    while (c.sent < c.out.size()) {
      const long n = sockets::Send(c.socket.Get(), c.out.data() + c.sent, c.out.size() - c.sent);
      if (n < 0) {
        return sockets::WouldBlock();
      }
      c.sent += static_cast<size_t>(n);
    }
    sockets::ShutdownBoth(c.socket.Get());
    return false;
  }

  void Run() {
    std::vector<std::unique_ptr<Connection>> connections;
    while (!stopping.load()) {
      std::vector<sockets::PollItem> items;
      items.push_back({.socket = listener.Get(), .want_read = true});
      for (const auto& c : connections) {
        items.push_back({.socket = c->socket.Get(), .want_read = !c->answering, .want_write = c->answering});
      }
      if (sockets::Poll(items, kPollMs) < 0) {
        logging::getLogger("pp-node").warning << "status HTTP poll: " << sockets::LastErrorText();
        continue;
      }
      if (items[0].readable) {
        while (true) {
          const sockets::Socket accepted = sockets::Accept(listener.Get());
          if (accepted == sockets::kInvalid) {
            break;
          }
          sockets::OwnedSocket socket(accepted);
          if (static_cast<int>(connections.size()) >= kMaxOpenConnections) {
            continue; // full: refuse at once (closed here) rather than queue
          }
          sockets::SetNonBlocking(socket.Get(), true);
          auto c = std::make_unique<Connection>();
          c->socket = std::move(socket);
          c->deadline = Clock::now() + kRequestDeadline;
          connections.push_back(std::move(c));
        }
      }
      const auto now = Clock::now();
      std::vector<std::unique_ptr<Connection>> kept;
      kept.reserve(connections.size());
      for (size_t i = 0; i < connections.size(); ++i) {
        Connection& c = *connections[i];
        // An item polled this round (a connection accepted just now was not).
        const sockets::PollItem* item = i + 1 < items.size() ? &items[i + 1] : nullptr;
        bool keep = true;
        if (item && item->socket == c.socket.Get()) {
          if (!c.answering && (item->readable || item->failed)) {
            keep = ReadFrom(c);
          }
          if (keep && c.answering && (item->writable || item->failed)) {
            keep = WriteTo(c);
          }
        }
        if (keep && !c.answering && now >= c.deadline) {
          keep = false; // a request too slow to arrive
        }
        if (keep) {
          kept.push_back(std::move(connections[i]));
        }
      }
      connections = std::move(kept);
    }
  }
};

StatusHttpServer::StatusHttpServer() = default;

StatusHttpServer::~StatusHttpServer() {
  Stop();
}

std::string StatusHttpServer::BoundEndpoint() const {
  if (!impl_) {
    return {};
  }
  return impl_->bound;
}

Roe<void> StatusHttpServer::Start(const StatusHttpBind& bind, StatusHttpAuthConfig auth, SnapshotFn snapshot,
                                  StatusHttpActions actions) {
  if (running_.load()) {
    return Error("status HTTP server already running");
  }
  if (!snapshot) {
    return Error("status HTTP snapshot callback required");
  }

  auto listener = sockets::Listen(bind.host, bind.port);
  if (!listener) {
    if (listener.error().message.rfind("not a numeric address", 0) == 0) {
      return Error("invalid status HTTP bind host: " + bind.host);
    }
    return Error("status HTTP " + listener.error().message);
  }
  auto impl = std::make_unique<Impl>();
  impl->listener.Reset(listener->socket);
  sockets::SetNonBlocking(impl->listener.Get(), true);
  impl->bound = listener->bound;
  impl->auth = std::move(auth);
  impl->snapshot = std::move(snapshot);
  impl->actions = std::move(actions);

  impl_ = std::move(impl);
  running_.store(true);

  auto* raw = impl_.get();
  thread_ = std::thread([this, raw]() {
    logging::getLogger("pp-node").info << "status HTTP listening on " << raw->bound << " (/healthz, /status, /metrics)";
    raw->Run();
    running_.store(false);
  });

  return {};
}

void StatusHttpServer::Stop() {
  if (!impl_) {
    return;
  }
  impl_->stopping.store(true);
  if (thread_.joinable()) {
    thread_.join();
  }
  impl_.reset();
  running_.store(false);
}

} // namespace pbr
