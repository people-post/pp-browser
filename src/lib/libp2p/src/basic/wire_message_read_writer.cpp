/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/basic/wire_message_read_writer.hpp>

#include <boost/assert.hpp>
#include <libp2p/basic/message_read_writer_uvarint.hpp>

namespace libp2p::basic {
  WireMessageReadWriter::WireMessageReadWriter(
      std::shared_ptr<MessageReadWriter> read_writer)
      : read_writer_{std::move(read_writer)} {
    BOOST_ASSERT(read_writer_);
  }

  WireMessageReadWriter::WireMessageReadWriter(std::shared_ptr<ReadWriter> conn)
      : read_writer_{
          std::make_shared<MessageReadWriterUvarint>(std::move(conn))} {
    BOOST_ASSERT(read_writer_);
  }
}  // namespace libp2p::basic
