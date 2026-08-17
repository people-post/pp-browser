/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <memory>

#include <libp2p/security/noise/handshake.hpp>

#include <libp2p/common/byteutil.hpp>
#include <libp2p/peer/peer_id.hpp>
#include <libp2p/security/noise/crypto/cipher_suite.hpp>
#include <libp2p/security/noise/crypto/noise_ccp1305.hpp>
#include <libp2p/security/noise/crypto/noise_dh.hpp>
#include <libp2p/security/noise/crypto/noise_sha256.hpp>
#include <libp2p/security/noise/crypto/state.hpp>
#include <libp2p/security/noise/handshake_message.hpp>
#include <libp2p/security/noise/noise_connection.hpp>

namespace libp2p::security::noise {

  namespace {
    template <typename T>
    void unused(T &&) {}
  }  // namespace

  std::shared_ptr<CipherSuite> defaultCipherSuite() {
    auto dh = std::make_shared<NoiseDiffieHellmanImpl>();
    auto hash = std::make_shared<NoiseSHA256HasherImpl>();
    auto cipher = std::make_shared<NamedCCPImpl>();
    return std::make_shared<CipherSuiteImpl>(
        std::move(dh), std::move(hash), std::move(cipher));
  }

  Handshake::Handshake(
      std::shared_ptr<crypto::CryptoProvider> crypto_provider,
      std::unique_ptr<security::noise::HandshakeMessageMarshaller>
          noise_marshaller,
      crypto::KeyPair local_key,
      std::shared_ptr<connection::LayerConnection> connection,
      bool is_initiator,
      boost::optional<peer::PeerId> remote_peer_id,
      SecurityAdaptor::SecConnCallbackFunc cb,
      std::shared_ptr<crypto::marshaller::KeyMarshaller> key_marshaller)
      : crypto_provider_{std::move(crypto_provider)},
        noise_marshaller_{std::move(noise_marshaller)},
        local_key_{std::move(local_key)},
        conn_{std::move(connection)},
        initiator_{is_initiator},
        connection_cb_{std::move(cb)},
        key_marshaller_{std::move(key_marshaller)},
        read_buffer_{std::make_shared<Bytes>(kMaxMsgLen)},
        rw_{std::make_shared<InsecureReadWriter>(conn_, read_buffer_)},
        handshake_state_{std::make_unique<HandshakeState>()},
        remote_peer_id_{std::move(remote_peer_id)} {
    read_buffer_->resize(kMaxMsgLen);
  }

  void Handshake::connect() {
    auto result = runHandshake();
    if (result.has_error()) {
      connection_cb_(result.error());
    }
  }

  void Handshake::setCipherStates(std::shared_ptr<CipherState> cs1,
                                  std::shared_ptr<CipherState> cs2) {
    if (initiator_) {
      enc_ = std::move(cs1);
      dec_ = std::move(cs2);
    } else {
      enc_ = std::move(cs2);
      dec_ = std::move(cs1);
    }
  }

  outcome::result<std::vector<uint8_t>> Handshake::generateHandshakePayload(
      const DHKey &keypair) {
    const auto &prefix = kPayloadPrefix;
    const auto &pubkey = keypair.pub;
    std::vector<uint8_t> to_sign;
    to_sign.reserve(prefix.size() + pubkey.size());
    std::copy(prefix.begin(), prefix.end(), std::back_inserter(to_sign));
    std::copy(pubkey.begin(), pubkey.end(), std::back_inserter(to_sign));

    auto signed_payload_res =
        crypto_provider_->sign(to_sign, local_key_.privateKey);
    if (!signed_payload_res) {
      return signed_payload_res.error();
    }
    auto signed_payload = std::move(signed_payload_res).value();
    security::noise::HandshakeMessage payload{
        .identity_key = local_key_.publicKey,
        .identity_sig = std::move(signed_payload),
        .data = {}};
    return noise_marshaller_->marshal(payload);
  }

  void Handshake::sendHandshakeMessage(BytesIn payload, CbOutcomeVoid cb) {
    auto write_result_res = handshake_state_->writeMessage({}, payload);
    if (write_result_res.has_error()) {
      cb(write_result_res.error());
      return;
    }
    auto write_result = std::move(write_result_res).value();
    auto write_cb = [self{shared_from_this()},
                     cb{std::move(cb)},
                     wr{write_result}](outcome::result<void> result) {
      if (result.has_error()) {
        return cb(result.error());
      }
      if (wr.cs1 and wr.cs2) {
        self->setCipherStates(wr.cs1, wr.cs2);
      }
      cb(outcome::success());
    };
    rw_->write(write_result.data, write_cb);
  }

  void Handshake::readHandshakeMessage(
      basic::MessageReadWriter::ReadCallbackFunc cb) {
    auto read_cb = [self{shared_from_this()}, cb{std::move(cb)}](auto result) {
      if (result.has_error()) {
        cb(result.error());
        return;
      }
      auto buffer = std::move(result).value();
      auto rr_res = self->handshake_state_->readMessage({}, *buffer);
      if (rr_res.has_error()) {
        cb(rr_res.error());
        return;
      }
      auto rr = std::move(rr_res).value();
      if (rr.cs1 and rr.cs2) {
        self->setCipherStates(rr.cs1, rr.cs2);
      }
      auto shared_data = std::make_shared<Bytes>();
      shared_data->swap(rr.data);
      cb(std::move(shared_data));
    };
    rw_->read(read_cb);
  }

  outcome::result<void> Handshake::handleRemoteHandshakePayload(
      BytesIn payload) {
    auto remote_payload_res = noise_marshaller_->unmarshal(payload);
    if (!remote_payload_res) {
      return remote_payload_res.as_failure();
    }
    auto remote_payload = std::move(remote_payload_res).value();
    auto remote_id_res = peer::PeerId::fromPublicKey(remote_payload.second);
    if (!remote_id_res) {
      return remote_id_res.as_failure();
    }
    auto remote_id = std::move(remote_id_res).value();
    auto &&handy_payload = remote_payload.first;
    if (initiator_ and remote_peer_id_ != remote_id) {
      SL_DEBUG(log_,
               "Remote peer id mismatches already known, expected {}, got {}",
               remote_peer_id_->toHex(),
               remote_id.toHex());
      return std::errc::bad_address;
    }
    Bytes to_verify;
    to_verify.reserve(kPayloadPrefix.size()
                      + handy_payload.identity_key.data.size());
    std::copy(kPayloadPrefix.begin(),
              kPayloadPrefix.end(),
              std::back_inserter(to_verify));
    auto remote_static_res = handshake_state_->remotePeerStaticPubkey();
    if (!remote_static_res) {
      return remote_static_res.as_failure();
    }
    auto remote_static = std::move(remote_static_res).value();
    std::copy(remote_static.begin(),
              remote_static.end(),
              std::back_inserter(to_verify));
    auto signature_correct_res = crypto_provider_->verify(
        to_verify, handy_payload.identity_sig, handy_payload.identity_key);
    if (!signature_correct_res) {
      return signature_correct_res.error();
    }
    auto signature_correct = std::move(signature_correct_res).value();
    if (not signature_correct) {
      SL_TRACE(log_, "Remote peer's payload signature verification failed");
      return std::errc::owner_dead;
    }
    remote_peer_id_ = remote_id;
    remote_peer_pubkey_ = handy_payload.identity_key;
    return outcome::success();
  }

  outcome::result<void> Handshake::runHandshake() {
    auto cipher_suite = defaultCipherSuite();
    auto keypair_res = cipher_suite->generate();
    if (!keypair_res) {
      return keypair_res.as_failure();
    }
    auto keypair = std::move(keypair_res).value();
    HandshakeStateConfig config(
        defaultCipherSuite(), handshakeXX, initiator_, keypair);
    auto init_res = handshake_state_->init(std::move(config));
    if (!init_res) {
      return init_res.as_failure();
    }
    auto payload_res = generateHandshakePayload(keypair);
    if (!payload_res) {
      return payload_res.as_failure();
    }
    auto payload = std::move(payload_res).value();
    if (initiator_) {
      //
      // Outgoing connection. Stage 0
      //
      SL_TRACE(log_, "outgoing connection. stage 0");
      sendHandshakeMessage(
          {},
          [self{shared_from_this()},
           payload{std::move(payload)}](outcome::result<void> result) {
            if (result.has_error()) {
              self->hscb(result.error());
              return;
            }
            //
            // Outgoing connection. Stage 1
            //
            SL_TRACE(self->log_, "outgoing connection. stage 1");
            self->readHandshakeMessage([self, payload](auto result) {
              if (result.has_error()) {
                self->hscb(result.error());
                return;
              }
              auto bytes_read = std::move(result).value();
              auto handle_result =
                  self->handleRemoteHandshakePayload(*bytes_read);
              if (handle_result.has_error()) {
                return self->hscb(handle_result.error());
              }
              //
              // Outgoing connection. Stage 2
              //
              SL_TRACE(self->log_, "outgoing connection. stage 2");
              self->sendHandshakeMessage(payload,
                                         [self, to_write(payload.size())](
                                             outcome::result<void> result) {
                                           if (result.has_error()) {
                                             self->hscb(result.error());
                                             return;
                                           }
                                           self->hscb(true);
                                         });
            });
          });
    } else {
      //
      // Incoming connection. Stage 0
      //
      SL_TRACE(log_, "incoming connection. stage 0");
      readHandshakeMessage(
          [self{shared_from_this()}, payload{std::move(payload)}](auto result) {
            if (result.has_error()) {
              self->hscb(result.error());
              return;
            }
            auto plaintext = std::move(result).value();
            unused(plaintext);
            /*
             * Seems that plaintext has to be ignored here. Probably we have to
             * check later that it has zero length.
             */
            //
            // Incoming connection. Stage 1
            //
            SL_TRACE(self->log_, "incoming connection. stage 1");
            self->sendHandshakeMessage(
                payload,
                [self, to_write(payload.size())](outcome::result<void> result) {
                  if (result.has_error()) {
                    self->hscb(result.error());
                    return;
                  }
                  //
                  // Incoming connection. Stage 2
                  //
                  SL_TRACE(self->log_, "incoming connection. stage 2");
                  self->readHandshakeMessage([self](auto result) {
                    if (result.has_error()) {
                      self->hscb(result.error());
                      return;
                    }
                    auto plaintext = std::move(result).value();
                    // may be need to check that plaintext is non empty
                    auto handle_result =
                        self->handleRemoteHandshakePayload(*plaintext);
                    if (handle_result.has_error()) {
                      self->hscb(handle_result.error());
                    }
                    self->hscb(true);
                  });
                });
          });
    }
    return outcome::success();
  }

  void Handshake::hscb(outcome::result<bool> secured) {
    if (secured.has_error()) {
      log_->error("handshake failed, {}", secured.error());
      return connection_cb_(secured.error());
    }
    if (not secured.value()) {
      log_->error("handshake failed for unknown reason");
      return connection_cb_(std::errc::io_error);
    }
    if (not remote_peer_pubkey_) {
      log_->error("Remote peer static pubkey remains unknown");
      return connection_cb_(std::errc::connection_aborted);
    }

    auto secured_connection = std::make_shared<connection::NoiseConnection>(
        conn_,
        local_key_.publicKey,
        remote_peer_pubkey_.value(),
        key_marshaller_,
        enc_,
        dec_);
    log_->info("Handshake succeeded");
    connection_cb_(std::move(secured_connection));
  }

}  // namespace libp2p::security::noise
