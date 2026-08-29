/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <functional>
#include <string>

#include <asio.hpp>
#include <system_error>

namespace libp2p::protocol::example::utility {

  /// Asio-based asynchronous line reader from stdin
  class ConsoleAsyncReader {
   public:
    /// lines read from the console come into this callback
    using Handler = std::function<void(const std::string &)>;

    /// starts the reader
    ConsoleAsyncReader(asio::io_context &io, Handler handler);

    /// stops the reader: no more callbacks after this call
    void stop();

   private:
    /// begins read operation
    void read();

    /// read callback from asio
    void onRead(const std::error_code &e, std::size_t size);

    asio::posix::stream_descriptor in_;
    asio::streambuf input_;
    std::string line_;
    Handler handler_;
    bool stopped_ = false;
  };

}  // namespace libp2p::protocol::example::utility
