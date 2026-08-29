/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <lsquic.h>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>
#include <deque>
#include <libp2p/multi/multiaddress.hpp>
#include <libp2p/peer/peer_id.hpp>
#include <memory>
#include <optional>
#include <qtils/bytes.hpp>
#include <qtils/outcome.hpp>

namespace asio {
  class io_context;
}  // namespace asio

namespace asio::ssl {
  class context;
}  // namespace asio::ssl

namespace libp2p::connection {
  struct QuicStream;
}  // namespace libp2p::connection

namespace libp2p::crypto::marshaller {
  class KeyMarshaller;
}  // namespace libp2p::crypto::marshaller

namespace libp2p::muxer {
  struct MuxedConnectionConfig;
}  // namespace libp2p::muxer

namespace libp2p::transport {
  struct QuicConnection;
}  // namespace libp2p::transport

namespace libp2p::transport::lsquic {
  using connection::QuicStream;

  struct Engine;
  struct ConnCtx;
  struct StreamCtx;

  using OnConnect =
      std::function<void(outcome::result<std::shared_ptr<QuicConnection>>)>;
  /**
   * Connect operation arguments.
   */
  struct Connecting {
    asio::ip::udp::endpoint remote;
    PeerId peer;
    OnConnect cb;
  };
  /**
   * `lsquic_conn_ctx_t` for libp2p connection.
   */
  struct ConnCtx {
    Engine *engine;
    lsquic_conn_t *ls_conn;
    std::optional<Connecting> connecting{};
    std::optional<std::shared_ptr<QuicStream>> new_stream{};
    std::weak_ptr<QuicConnection> conn{};
  };

  /**
   * `lsquic_stream_ctx_t` for libp2p stream.
   */
  struct StreamCtx {
    Engine *engine;
    lsquic_stream_t *ls_stream;
    std::weak_ptr<QuicStream> stream{};
    std::optional<std::function<void()>> reading{};
    std::optional<std::function<void()>> writing{};
    bool want_flush = false;
  };

  using OnAccept = std::function<void(std::shared_ptr<QuicConnection>)>;

  /**
   * libp2p wrapper and adapter for lsquic server/client socket.
   */
  class Engine : public std::enable_shared_from_this<Engine> {
   public:
    Engine(std::shared_ptr<asio::io_context> io_context,
           std::shared_ptr<asio::ssl::context> ssl_context,
           const muxer::MuxedConnectionConfig &mux_config,
           PeerId local_peer,
           std::shared_ptr<crypto::marshaller::KeyMarshaller> key_codec,
           asio::ip::udp::socket &&socket,
           bool client);
    ~Engine();

    // clang-tidy cppcoreguidelines-special-member-functions
    Engine(const Engine &) = delete;
    void operator=(const Engine &) = delete;
    Engine(Engine &&) = delete;
    void operator=(Engine &&) = delete;

    auto &local() const {
      return local_;
    }
    void start();
    void connect(const asio::ip::udp::endpoint &remote,
                 const PeerId &peer,
                 OnConnect cb);
    outcome::result<std::shared_ptr<QuicStream>> newStream(ConnCtx *conn_ctx);
    void onAccept(OnAccept cb) {
      on_accept_ = std::move(cb);
    }
    void wantProcess();
    void wantFlush(StreamCtx *stream_ctx);

   private:
    void process();
    void readLoop();

    std::shared_ptr<asio::io_context> io_context_;
    std::shared_ptr<asio::ssl::context> ssl_context_;
    PeerId local_peer_;
    std::shared_ptr<crypto::marshaller::KeyMarshaller> key_codec_;
    asio::ip::udp::socket socket_;
    asio::steady_timer timer_;
    asio::ip::udp::endpoint socket_local_;
    Multiaddress local_;
    lsquic_engine_t *engine_ = nullptr;
    OnAccept on_accept_;
    bool started_ = false;
    std::deque<std::weak_ptr<connection::QuicStream>> want_flush_;
    bool want_process_ = false;
    std::optional<Connecting> connecting_;
    struct Reading {
      static constexpr size_t kMaxUdpPacketSize = 64 << 10;
      qtils::BytesN<kMaxUdpPacketSize> buf;
      asio::ip::udp::endpoint remote;
    };
    Reading reading_;
  };
}  // namespace libp2p::transport::lsquic
