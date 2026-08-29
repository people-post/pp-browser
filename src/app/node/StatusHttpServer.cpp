#include "app/node/StatusHttpServer.h"

#include "common/Logger.h"

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <system_error>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

using tcp = asio::ip::tcp;
constexpr size_t kMaxRequestBytes = 16 * 1024;

void HandleConnection(std::shared_ptr<tcp::socket> socket, StatusHttpAuthConfig auth,
                      StatusHttpServer::SnapshotFn snapshot) {
  auto buf = std::make_shared<std::string>();
  buf->reserve(1024);
  auto chunk = std::make_shared<std::array<char, 1024>>();

  struct Reader : std::enable_shared_from_this<Reader> {
    std::shared_ptr<tcp::socket> socket;
    StatusHttpAuthConfig auth;
    StatusHttpServer::SnapshotFn snapshot;
    std::shared_ptr<std::string> buf;
    std::shared_ptr<std::array<char, 1024>> chunk;

    void Start() { ReadMore(); }

    void ReadMore() {
      auto self = shared_from_this();
      socket->async_read_some(
          asio::buffer(*chunk),
          [self](const std::error_code& rec, std::size_t n) {
            if (rec || n == 0) {
              std::error_code ignored;
              self->socket->shutdown(tcp::socket::shutdown_both, ignored);
              self->socket->close(ignored);
              return;
            }
            self->buf->append(self->chunk->data(), n);
            if (self->buf->size() > kMaxRequestBytes) {
              std::error_code ignored;
              self->socket->close(ignored);
              return;
            }
            if (auto req = TryParseStatusHttpRequest(*self->buf)) {
              StatusHttpSnapshot snap;
              try {
                snap = self->snapshot ? self->snapshot() : StatusHttpSnapshot{};
              } catch (...) {
                snap = StatusHttpSnapshot{};
              }
              auto response = HandleStatusHttpRequest(*req, self->auth, snap);
              if (req->method == "HEAD") {
                response.body.clear();
              }
              auto out = std::make_shared<std::string>(FormatStatusHttpResponse(response));
              asio::async_write(
                  *self->socket, asio::buffer(*out),
                  [self, out](const std::error_code&, std::size_t) {
                    std::error_code ignored;
                    self->socket->shutdown(tcp::socket::shutdown_both, ignored);
                    self->socket->close(ignored);
                  });
              return;
            }
            self->ReadMore();
          });
    }
  };

  auto reader = std::make_shared<Reader>();
  reader->socket = std::move(socket);
  reader->auth = std::move(auth);
  reader->snapshot = std::move(snapshot);
  reader->buf = std::move(buf);
  reader->chunk = std::move(chunk);
  reader->Start();
}

} // namespace

struct StatusHttpServer::Impl {
  asio::io_context io;
  std::unique_ptr<tcp::acceptor> acceptor;
  StatusHttpAuthConfig auth;
  SnapshotFn snapshot;
  std::string bound;
  std::atomic<bool> stopping{false};

  void ScheduleAccept() {
    if (stopping.load() || !acceptor) {
      return;
    }
    auto socket = std::make_shared<tcp::socket>(io);
    acceptor->async_accept(*socket, [this, socket](const std::error_code& ec) {
      if (stopping.load()) {
        return;
      }
      if (!ec) {
        HandleConnection(socket, auth, snapshot);
      } else if (ec != asio::error::operation_aborted) {
        logging::getLogger("pp-node").warning << "status HTTP accept: " << ec.message();
      }
      ScheduleAccept();
    });
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

Roe<void> StatusHttpServer::Start(const StatusHttpBind& bind, StatusHttpAuthConfig auth,
                                  SnapshotFn snapshot) {
  if (running_.load()) {
    return Error("status HTTP server already running");
  }
  if (!snapshot) {
    return Error("status HTTP snapshot callback required");
  }

  auto impl = std::make_unique<Impl>();
  impl->auth = std::move(auth);
  impl->snapshot = std::move(snapshot);

  std::error_code ec;
  const auto address = asio::ip::make_address(bind.host, ec);
  if (ec) {
    return Error("invalid status HTTP bind host: " + bind.host);
  }
  tcp::endpoint endpoint(address, bind.port);
  impl->acceptor = std::make_unique<tcp::acceptor>(impl->io);
  impl->acceptor->open(endpoint.protocol(), ec);
  if (ec) {
    return Error("status HTTP open failed: " + ec.message());
  }
  impl->acceptor->set_option(tcp::acceptor::reuse_address(true), ec);
  if (ec) {
    return Error("status HTTP set_option failed: " + ec.message());
  }
  impl->acceptor->bind(endpoint, ec);
  if (ec) {
    return Error("status HTTP bind failed: " + ec.message());
  }
  impl->acceptor->listen(asio::socket_base::max_listen_connections, ec);
  if (ec) {
    return Error("status HTTP listen failed: " + ec.message());
  }

  const auto local = impl->acceptor->local_endpoint(ec);
  if (!ec) {
    impl->bound = local.address().to_string() + ":" + std::to_string(local.port());
  } else {
    impl->bound = bind.host + ":" + std::to_string(bind.port);
  }

  impl_ = std::move(impl);
  running_.store(true);

  auto* raw = impl_.get();
  thread_ = std::thread([this, raw]() {
    raw->ScheduleAccept();
    logging::getLogger("pp-node").info << "status HTTP listening on " << raw->bound
                                       << " (/healthz, /status)";
    raw->io.run();
    running_.store(false);
  });

  return {};
}

void StatusHttpServer::Stop() {
  if (!impl_) {
    return;
  }
  impl_->stopping.store(true);
  std::error_code ec;
  if (impl_->acceptor) {
    impl_->acceptor->close(ec);
  }
  impl_->io.stop();
  if (thread_.joinable()) {
    thread_.join();
  }
  impl_.reset();
  running_.store(false);
}

} // namespace pbr
