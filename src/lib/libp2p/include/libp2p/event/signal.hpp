/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace libp2p::event {

  /**
   * Disconnectable subscription ticket for @ref Signal.
   * Movable; copy is deleted (matches prior boost::signals2::connection usage).
   */
  class Connection {
   public:
    Connection() = default;

    explicit Connection(std::function<void()> disconnect)
        : disconnect_(std::move(disconnect)),
          connected_(static_cast<bool>(disconnect_)) {}

    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    Connection(Connection &&other) noexcept
        : disconnect_(std::move(other.disconnect_)),
          connected_(other.connected_) {
      other.connected_ = false;
      other.disconnect_ = nullptr;
    }

    Connection &operator=(Connection &&other) noexcept {
      if (this == &other) {
        return *this;
      }
      disconnect();
      disconnect_ = std::move(other.disconnect_);
      connected_ = other.connected_;
      other.connected_ = false;
      other.disconnect_ = nullptr;
      return *this;
    }

    ~Connection() = default;

    bool connected() const {
      return connected_;
    }

    void disconnect() {
      if (!connected_) {
        return;
      }
      connected_ = false;
      if (disconnect_) {
        auto fn = std::move(disconnect_);
        disconnect_ = nullptr;
        fn();
      }
    }

   private:
    std::function<void()> disconnect_;
    bool connected_ = false;
  };

  /**
   * RAII connection that disconnects on destruction (scoped_connection analogue).
   */
  class ScopedConnection {
   public:
    ScopedConnection() = default;
    explicit ScopedConnection(Connection conn) : conn_(std::move(conn)) {}

    ScopedConnection(const ScopedConnection &) = delete;
    ScopedConnection &operator=(const ScopedConnection &) = delete;

    ScopedConnection(ScopedConnection &&other) noexcept
        : conn_(std::move(other.conn_)) {}

    ScopedConnection &operator=(ScopedConnection &&other) noexcept {
      if (this == &other) {
        return *this;
      }
      disconnect();
      conn_ = std::move(other.conn_);
      return *this;
    }

    ~ScopedConnection() {
      disconnect();
    }

    bool connected() const {
      return conn_.connected();
    }

    void disconnect() {
      conn_.disconnect();
    }

   private:
    Connection conn_;
  };

  /**
   * Minimal multicast signal replacing boost::signals2::signal.
   * @tparam Signature function type, e.g. void(const T &)
   */
  template <typename Signature>
  class Signal;

  template <typename... Args>
  class Signal<void(Args...)> {
   public:
    using Slot = std::function<void(Args...)>;

    Signal() = default;
    Signal(const Signal &) = delete;
    Signal &operator=(const Signal &) = delete;
    Signal(Signal &&) = default;
    Signal &operator=(Signal &&) = default;

    template <typename F>
    Connection connect(F &&slot) {
      const auto id = next_id_++;
      auto shared = std::make_shared<Slot>(std::forward<F>(slot));
      entries_.push_back(Entry{id, std::move(shared)});
      return Connection([this, id] { disconnect(id); });
    }

    void operator()(Args... args) const {
      // Snapshot so disconnect during emit is safe.
      auto snapshot = entries_;
      for (const auto &entry : snapshot) {
        if (entry.slot) {
          (*entry.slot)(args...);
        }
      }
    }

    std::size_t num_slots() const {
      return entries_.size();
    }

    bool empty() const {
      return entries_.empty();
    }

   private:
    struct Entry {
      uint64_t id = 0;
      std::shared_ptr<Slot> slot;
    };

    void disconnect(uint64_t id) {
      for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->id == id) {
          entries_.erase(it);
          return;
        }
      }
    }

    uint64_t next_id_ = 1;
    std::vector<Entry> entries_;
  };

}  // namespace libp2p::event
