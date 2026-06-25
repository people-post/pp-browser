/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/security/secio/secio_connection.hpp>

#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
#include <libp2p/basic/read.hpp>
#include <libp2p/basic/write.hpp>
#include <libp2p/common/byteutil.hpp>
#include <libp2p/crypto/aes_ctr/aes_ctr_impl.hpp>
#include <libp2p/crypto/error.hpp>
#include <libp2p/crypto/hmac_provider.hpp>

OUTCOME_CPP_DEFINE_CATEGORY(libp2p::connection, SecioConnection::Error, e) {
  using E = libp2p::connection::SecioConnection::Error;
  switch (e) {
    case E::CONN_NOT_INITIALIZED:
      return "Connection is not initialized";
    case E::CONN_ALREADY_INITIALIZED:
      return "Connection was already initialized";
    case E::INITALIZATION_FAILED:
      return "Connection initialization failed";
    case E::UNSUPPORTED_CIPHER:
      return "Unsupported cipher algorithm is used";
    case E::UNSUPPORTED_HASH:
      return "Unsupported hash algorithm is used";
    case E::INVALID_MAC:
      return "Received message has an invalid signature";
    case E::TOO_SHORT_BUFFER:
      return "Provided buffer is too short";
    case E::NOTHING_TO_READ:
      return "Zero bytes available to read";
    case E::STREAM_IS_BROKEN:
      return "Stream cannot be read. Frames are corrupted";
    case E::OVERSIZED_FRAME:
      return "Frame exceeds the maximum allowed size";
    default:
      return "Unknown error";
  }
}

namespace {
  template <typename AesSecretType>
  outcome::result<AesSecretType> initAesSecret(const libp2p::Bytes &key,
                                               const libp2p::Bytes &iv) {
    AesSecretType secret{};
    if (key.size() != secret.key.size()) {
      return libp2p::crypto::OpenSslError::WRONG_KEY_SIZE;
    }
    if (iv.size() != secret.iv.size()) {
      return libp2p::crypto::OpenSslError::WRONG_IV_SIZE;
    }
    std::copy(key.begin(), key.end(), secret.key.begin());
    std::copy(iv.begin(), iv.end(), secret.iv.begin());
    return secret;
  }
}  // namespace

namespace libp2p::connection {

  SecioConnection::SecioConnection(
      std::shared_ptr<LayerConnection> original_connection,
      std::shared_ptr<crypto::hmac::HmacProvider> hmac_provider,
      std::shared_ptr<crypto::marshaller::KeyMarshaller> key_marshaller,
      crypto::PublicKey local_pubkey,
      crypto::PublicKey remote_pubkey,
      crypto::common::HashType hash_type,
      crypto::common::CipherType cipher_type,
      crypto::StretchedKey local_stretched_key,
      crypto::StretchedKey remote_stretched_key)
      : original_connection_{std::move(original_connection)},
        hmac_provider_{std::move(hmac_provider)},
        key_marshaller_{std::move(key_marshaller)},
        local_{std::move(local_pubkey)},
        remote_{std::move(remote_pubkey)},
        hash_type_{hash_type},
        cipher_type_{cipher_type},
        local_stretched_key_{std::move(local_stretched_key)},
        remote_stretched_key_{std::move(remote_stretched_key)},
        aes128_secrets_{boost::none},
        aes256_secrets_{boost::none},
        write_buffer_{std::make_shared<Bytes>()} {
    BOOST_ASSERT(original_connection_);
    BOOST_ASSERT(hmac_provider_);
    BOOST_ASSERT(key_marshaller_);
  }

  outcome::result<void> SecioConnection::init() {
    if (isInitialized()) {
      return Error::CONN_ALREADY_INITIALIZED;
    }

    using CT = crypto::common::CipherType;
    using AesCtrMode = crypto::aes::AesCtrImpl::Mode;
    if (cipher_type_ == CT::AES128) {
      auto local_128_res = initAesSecret<crypto::common::Aes128Secret>(
          local_stretched_key_.cipher_key, local_stretched_key_.iv);
      if (!local_128_res) {
        return local_128_res.error();
      }
      auto local_128 = std::move(local_128_res).value();
      auto remote_128_res = initAesSecret<crypto::common::Aes128Secret>(
          remote_stretched_key_.cipher_key, remote_stretched_key_.iv);
      if (!remote_128_res) {
        return remote_128_res.error();
      }
      auto remote_128 = std::move(remote_128_res).value();
      local_encryptor_ = std::make_unique<crypto::aes::AesCtrImpl>(
          local_128, AesCtrMode::ENCRYPT);
      remote_decryptor_ = std::make_unique<crypto::aes::AesCtrImpl>(
          remote_128, AesCtrMode::DECRYPT);
    } else if (cipher_type_ == CT::AES256) {
      auto local_256_res = initAesSecret<crypto::common::Aes256Secret>(
          local_stretched_key_.cipher_key, local_stretched_key_.iv);
      if (!local_256_res) {
        return local_256_res.error();
      }
      auto local_256 = std::move(local_256_res).value();
      auto remote_256_res = initAesSecret<crypto::common::Aes256Secret>(
          remote_stretched_key_.cipher_key, remote_stretched_key_.iv);
      if (!remote_256_res) {
        return remote_256_res.error();
      }
      auto remote_256 = std::move(remote_256_res).value();
      local_encryptor_ = std::make_unique<crypto::aes::AesCtrImpl>(
          local_256, AesCtrMode::ENCRYPT);
      remote_decryptor_ = std::make_unique<crypto::aes::AesCtrImpl>(
          remote_256, AesCtrMode::DECRYPT);
    } else {
      return Error::UNSUPPORTED_CIPHER;
    }

    read_buffer_ = std::make_shared<Bytes>(kMaxFrameSize);

    return outcome::success();
  }

  bool SecioConnection::isInitialized() const {
    return local_encryptor_ and remote_decryptor_;
  }

  outcome::result<peer::PeerId> SecioConnection::localPeer() const {
    auto proto_local_key_res = key_marshaller_->marshal(local_);
    if (!proto_local_key_res) {
      return proto_local_key_res.as_failure();
    }
    auto proto_local_key = std::move(proto_local_key_res).value();
    return peer::PeerId::fromPublicKey(proto_local_key);
  }

  outcome::result<peer::PeerId> SecioConnection::remotePeer() const {
    auto proto_remote_key_res = key_marshaller_->marshal(remote_);
    if (!proto_remote_key_res) {
      return proto_remote_key_res.as_failure();
    }
    auto proto_remote_key = std::move(proto_remote_key_res).value();
    return peer::PeerId::fromPublicKey(proto_remote_key);
  }

  outcome::result<crypto::PublicKey> SecioConnection::remotePublicKey() const {
    return remote_;
  }

  bool SecioConnection::isInitiator() const {
    return original_connection_->isInitiator();
  }

  outcome::result<multi::Multiaddress> SecioConnection::localMultiaddr() {
    return original_connection_->localMultiaddr();
  }

  outcome::result<multi::Multiaddress> SecioConnection::remoteMultiaddr() {
    return original_connection_->remoteMultiaddr();
  }

  void SecioConnection::deferReadCallback(outcome::result<size_t> res,
                                          ReadCallbackFunc cb) {
    original_connection_->deferReadCallback(res, std::move(cb));
  }

  void SecioConnection::deferWriteCallback(std::error_code ec,
                                           WriteCallbackFunc cb) {
    original_connection_->deferWriteCallback(ec, std::move(cb));
  }

  inline void SecioConnection::popUserData(BytesOut out, size_t bytes) {
    auto to{out.begin()};
    for (size_t read = 0; read < bytes; ++read) {
      *to = user_data_buffer_.front();
      user_data_buffer_.pop();
      ++to;
    }
  }

  void SecioConnection::readSome(BytesOut out,
                                 basic::Reader::ReadCallbackFunc cb) {
    // TODO(107): Reentrancy

    if (!isInitialized()) {
      cb(Error::CONN_NOT_INITIALIZED);
      return;
    }

    if (not user_data_buffer_.empty()) {
      size_t to_read{std::min(user_data_buffer_.size(), out.size())};
      popUserData(out, to_read);
      SL_TRACE(log_, "Successfully read {} bytes", to_read);
      cb(to_read);
      return;
    }

    readNextMessage([self{shared_from_this()}, out, cb{std::move(cb)}](
                        outcome::result<void> result) {
      if (result.has_error()) {
        return cb(result.error());
      }
      self->readSome(out, std::move(cb));
    });
  }

  void SecioConnection::readNextMessage(CbOutcomeVoid cb) {
    read_buffer_->resize(kLenMarkerSize);
    libp2p::read(
        original_connection_,
        *read_buffer_,
        [self{shared_from_this()}, buffer = read_buffer_, cb{std::move(cb)}](
            outcome::result<void> result) mutable {
          if (result.has_error()) {
            return cb(result.error());
          }
          uint32_t frame_len{
              ntohl(common::convert<uint32_t>(buffer->data()))};  // NOLINT
          if (frame_len > kMaxFrameSize) {
            self->log_->error("Frame size {} exceeds maximum allowed size {}",
                              frame_len,
                              kMaxFrameSize);
            cb(Error::OVERSIZED_FRAME);
            return;
          }
          SL_TRACE(self->log_, "Expecting frame of size {}.", frame_len);
          buffer->resize(frame_len);
          libp2p::read(
              self->original_connection_,
              *buffer,
              [self, buffer, frame_len, cb{std::move(cb)}](
                  outcome::result<void> result) mutable {
                if (result.has_error()) {
                  return cb(result.error());
                }
                SL_TRACE(self->log_, "Received frame with len {}", frame_len);
                auto mac_size_result = self->macSize();
                if (mac_size_result.has_error()) {
                  return cb(mac_size_result.error());
                }
                auto mac_size = std::move(mac_size_result).value();
                const auto data_size{frame_len - mac_size};
                auto data_span{std::span(buffer->data(), data_size)};
                auto mac_span{std::span(*buffer).subspan(data_size, mac_size)};
                auto remote_mac_result = self->macRemote(data_span);
                if (remote_mac_result.has_error()) {
                  return cb(remote_mac_result.error());
                }
                auto remote_mac = std::move(remote_mac_result).value();
                if (BytesIn(remote_mac) != BytesIn(mac_span)) {
                  self->log_->error(
                      "Signature does not validate for the received frame");
                  cb(Error::INVALID_MAC);
                  return;
                }
                auto decrypted_result =
                    (*self->remote_decryptor_)->crypt(data_span);
                if (decrypted_result.has_error()) {
                  return cb(decrypted_result.error());
                }
                auto decrypted_bytes = std::move(decrypted_result).value();
                size_t decrypted_bytes_len{decrypted_bytes.size()};
                for (auto &&e : decrypted_bytes) {
                  self->user_data_buffer_.emplace(std::forward<decltype(e)>(e));
                }
                SL_TRACE(self->log_,
                         "Frame decrypted successfully {} -> {}",
                         frame_len,
                         decrypted_bytes_len);
                cb(outcome::success());
              });
        });
  }

  void SecioConnection::writeSome(BytesIn in,
                                  basic::Writer::WriteCallbackFunc cb) {
    // TODO(107): Reentrancy

    if (!isInitialized()) {
      cb(Error::CONN_NOT_INITIALIZED);
    }
    auto mac_size_result = macSize();
    if (mac_size_result.has_error()) {
      return cb(mac_size_result.error());
    }
    auto mac_size = std::move(mac_size_result).value();
    size_t frame_len{in.size() + mac_size};
    write_buffer_->resize(0);
    auto &frame_buffer = *write_buffer_;
    constexpr size_t len_field_size{kLenMarkerSize};
    frame_buffer.reserve(len_field_size + frame_len);

    common::putUint32BE(frame_buffer, frame_len);
    auto encrypted_result = (*local_encryptor_)->crypt(in);
    if (encrypted_result.has_error()) {
      return cb(encrypted_result.error());
    }
    auto encrypted_data = std::move(encrypted_result).value();

    auto mac_result = macLocal(encrypted_data);
    if (mac_result.has_error()) {
      return cb(mac_result.error());
    }
    auto mac_data = std::move(mac_result).value();
    frame_buffer.insert(frame_buffer.end(),
                        std::make_move_iterator(encrypted_data.begin()),
                        std::make_move_iterator(encrypted_data.end()));
    frame_buffer.insert(frame_buffer.end(),
                        std::make_move_iterator(mac_data.begin()),
                        std::make_move_iterator(mac_data.end()));
    write(original_connection_,
          frame_buffer,
          [buffer{write_buffer_}, in, cb{std::move(cb)}](
              outcome::result<void> result) {
            if (result.has_error()) {
              return cb(result.error());
            }
            cb(in.size());
          });
  }

  bool SecioConnection::isClosed() const {
    return original_connection_->isClosed();
  }

  outcome::result<void> SecioConnection::close() {
    return original_connection_->close();
  }

  outcome::result<size_t> SecioConnection::macSize() const {
    using HT = crypto::common::HashType;
    switch (hash_type_) {
      case HT::SHA256:
        return 32;
      case HT::SHA512:
        return 64;
      default:
        return Error::UNSUPPORTED_HASH;
    }
  }

  outcome::result<Bytes> SecioConnection::macLocal(BytesIn message) const {
    return hmac_provider_->calculateDigest(
        hash_type_, local_stretched_key_.mac_key, message);
  }

  outcome::result<Bytes> SecioConnection::macRemote(BytesIn message) const {
    return hmac_provider_->calculateDigest(
        hash_type_, remote_stretched_key_.mac_key, message);
  }

}  // namespace libp2p::connection
