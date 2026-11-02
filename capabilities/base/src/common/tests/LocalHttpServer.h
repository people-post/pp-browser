#pragma once

#include <asio/ip/address_v4.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace pbr::test {

class LocalHttpServer {
public:
  LocalHttpServer() : acceptor_(io_) {
    std::error_code ec;
    const asio::ip::tcp::endpoint endpoint(asio::ip::address_v4::loopback(), 0);
    acceptor_.open(endpoint.protocol(), ec);
    if (!ec) {
      acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
    }
    if (!ec) {
      acceptor_.bind(endpoint, ec);
    }
    if (!ec) {
      acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    }
    if (ec) {
      throw std::runtime_error("local HTTP server setup failed: " + ec.message());
    }
    port_ = acceptor_.local_endpoint().port();
    ScheduleAccept();
    thread_ = std::thread([this] { io_.run(); });
  }

  ~LocalHttpServer() {
    stopping_.store(true);
    std::error_code ignored;
    acceptor_.close(ignored);
    io_.stop();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  LocalHttpServer(const LocalHttpServer&) = delete;
  LocalHttpServer& operator=(const LocalHttpServer&) = delete;

  void SetResponse(std::string body) {
    std::lock_guard lock(response_mutex_);
    response_ = std::move(body);
  }

  std::string Url() const { return "http://127.0.0.1:" + std::to_string(port_); }

private:
  using tcp = asio::ip::tcp;

  struct Connection {
    explicit Connection(tcp::socket socket) : socket(std::move(socket)) {}

    tcp::socket socket;
    asio::streambuf request;
    std::string response;
  };

  void ScheduleAccept() {
    if (stopping_.load()) {
      return;
    }
    auto socket = std::make_shared<tcp::socket>(io_);
    acceptor_.async_accept(*socket, [this, socket](const std::error_code& ec) {
      if (!stopping_.load() && !ec) {
        Respond(std::move(*socket));
      }
      ScheduleAccept();
    });
  }

  void Respond(tcp::socket socket) {
    auto connection = std::make_shared<Connection>(std::move(socket));
    asio::async_read_until(
        connection->socket, connection->request, "\r\n\r\n",
        [this, connection](const std::error_code& ec, std::size_t) {
          if (ec || stopping_.load()) {
            return;
          }

          std::string body;
          {
            std::lock_guard lock(response_mutex_);
            body = response_;
          }
          connection->response = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) + "\r\n"
                                 "Connection: close\r\n\r\n" + body;
          asio::async_write(connection->socket, asio::buffer(connection->response),
                            [connection](const std::error_code&, std::size_t) {
                              std::error_code ignored;
                              connection->socket.shutdown(tcp::socket::shutdown_both, ignored);
                              connection->socket.close(ignored);
                            });
        });
  }

  asio::io_context io_;
  tcp::acceptor acceptor_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  mutable std::mutex response_mutex_;
  std::string response_;
  unsigned short port_ = 0;
};

} // namespace pbr::test
