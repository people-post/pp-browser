#include "base/p2p/StreamJsonFrame.h"

#include "base/p2p/StreamFrameIo.h"

#include <libp2p/basic/write.hpp>

#include <algorithm>
#include <utility>

namespace pbr {

Roe<std::vector<uint8_t>> EncodeStreamJsonFrame(const std::string& json_utf8) {
  if (json_utf8.size() > kMaxStreamJsonFrameBytes) {
    return Error("json frame too large");
  }
  std::vector<uint8_t> frame(8 + json_utf8.size());
  uint64_t len = json_utf8.size();
  for (int i = 7; i >= 0; --i) {
    frame[static_cast<size_t>(i)] = static_cast<uint8_t>(len & 0xff);
    len >>= 8;
  }
  std::copy(json_utf8.begin(), json_utf8.end(), frame.begin() + 8);
  return frame;
}

Roe<std::string> DecodeStreamJsonFrame(const std::vector<uint8_t>& frame_bytes) {
  if (frame_bytes.size() < 8) {
    return Error("json frame truncated");
  }
  uint64_t payload_len = 0;
  for (size_t i = 0; i < 8; ++i) {
    payload_len = (payload_len << 8) | frame_bytes[i];
  }
  if (payload_len > kMaxStreamJsonFrameBytes || frame_bytes.size() != 8 + payload_len) {
    return Error("json frame length mismatch");
  }
  return std::string(frame_bytes.begin() + 8, frame_bytes.end());
}

Roe<std::string> BlockingReadStreamJson(const std::shared_ptr<libp2p::connection::Stream>& stream,
                                        const size_t max_frame_bytes,
                                        const std::chrono::milliseconds read_timeout) {
  LengthPrefixedFrameConfig config;
  config.max_frame_bytes = max_frame_bytes;
  config.read_timeout = read_timeout;
  auto body = BlockingReadLengthPrefixedFrame(stream, config);
  if (!body) {
    return body.error();
  }
  return std::string(body->begin(), body->end());
}

Roe<void> BlockingWriteStreamJson(const std::shared_ptr<libp2p::connection::Stream>& stream,
                                  const std::string& json_utf8, const size_t max_frame_bytes) {
  if (json_utf8.size() > max_frame_bytes) {
    return Error("json frame too large");
  }
  std::vector<uint8_t> body(json_utf8.begin(), json_utf8.end());
  return BlockingWriteLengthPrefixedFrame(stream, body);
}

void AsyncReadStreamJson(std::shared_ptr<libp2p::connection::Stream> stream,
                         StreamJsonReadCallback on_done, StreamCancelCheck is_cancelled,
                         const size_t max_frame_bytes) {
  LengthPrefixedFrameConfig config;
  config.max_frame_bytes = max_frame_bytes;
  AsyncReadStreamJson(std::move(stream), std::move(on_done), std::move(is_cancelled),
                      std::move(config));
}

void AsyncReadStreamJson(std::shared_ptr<libp2p::connection::Stream> stream,
                         StreamJsonReadCallback on_done, StreamCancelCheck is_cancelled,
                         LengthPrefixedFrameConfig config) {
  if (!stream || !on_done) {
    if (on_done) {
      on_done(Error("async json read: missing stream"));
    }
    return;
  }
  if (is_cancelled && is_cancelled()) {
    on_done(Error("async json read cancelled"));
    return;
  }
  auto reader = std::make_shared<AsyncLengthPrefixedReader>();
  reader->Start(
      std::move(stream),
      [reader, on_done = std::move(on_done)](Roe<std::vector<uint8_t>> body) mutable {
        reader->Stop();
        if (!body) {
          on_done(body.error());
          return;
        }
        on_done(std::string(body->begin(), body->end()));
      },
      std::move(is_cancelled), std::move(config));
}

void AsyncWriteStreamJson(std::shared_ptr<libp2p::connection::Stream> stream, std::string json_utf8,
                          StreamJsonWriteCallback on_done, const size_t max_frame_bytes) {
  if (!stream) {
    if (on_done) {
      on_done(Error("async json write: missing stream"));
    }
    return;
  }
  if (json_utf8.size() > max_frame_bytes) {
    if (on_done) {
      on_done(Error("json frame too large"));
    }
    return;
  }
  std::vector<uint8_t> body(json_utf8.begin(), json_utf8.end());
  auto frame = std::make_shared<std::vector<uint8_t>>(EncodeLengthPrefixedFrame(body));
  libp2p::write(stream, *frame,
                [stream, frame, on_done = std::move(on_done)](outcome::result<void> result) mutable {
                  if (!on_done) {
                    return;
                  }
                  if (!result) {
                    on_done(Error("Failed to write length-prefixed frame"));
                    return;
                  }
                  on_done({});
                });
}

} // namespace pbr
