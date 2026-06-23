/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/security/secio/secio.hpp>

#include <generated/security/secio/protobuf/secio.pb.h>
#include <libp2p/basic/protobuf_message_read_writer.hpp>
#include <libp2p/basic/read.hpp>
#include <libp2p/basic/write.hpp>
#include <libp2p/crypto/sha/sha256.hpp>
#include <libp2p/security/error.hpp>
#include <libp2p/security/secio/secio_connection.hpp>
#include <libp2p/security/secio/secio_dialer.hpp>

OUTCOME_CPP_DEFINE_CATEGORY(libp2p::security, Secio::Error, e) {
  using E = libp2p::security::Secio::Error;
  switch (e) {  // NOLINT
    case E::REMOTE_PEER_SIGNATURE_IS_INVALID:
      return "Remote peer exchange message contains invalid signature";
    case E::INITIAL_PACKET_VERIFICATION_FAILED:
      return "Error happened while initial packet verification";
    default:
      return "Unknown error";
  }
}

namespace libp2p::security {

  Secio::Secio(
      std::shared_ptr<crypto::random::CSPRNG> csprng,
      std::shared_ptr<crypto::CryptoProvider> crypto_provider,
      std::shared_ptr<secio::ProposeMessageMarshaller> propose_marshaller,
      std::shared_ptr<secio::ExchangeMessageMarshaller> exchange_marshaller,
      std::shared_ptr<peer::IdentityManager> idmgr,
      std::shared_ptr<crypto::marshaller::KeyMarshaller> key_marshaller,
      std::shared_ptr<crypto::hmac::HmacProvider> hmac_provider)
      : csprng_(std::move(csprng)),
        crypto_provider_(std::move(crypto_provider)),
        propose_marshaller_(std::move(propose_marshaller)),
        exchange_marshaller_(std::move(exchange_marshaller)),
        idmgr_(std::move(idmgr)),
        key_marshaller_(std::move(key_marshaller)),
        hmac_provider_(std::move(hmac_provider)),
        propose_message_{.rand = csprng_->randomBytes(16),
                         .pubkey = {},  // marshalled public key will be stored
                                        // here, initialized in constructor body
                         .exchanges = kExchanges,
                         .ciphers = kCiphers,
                         .hashes = kHashes} {
    BOOST_ASSERT(csprng_);
    BOOST_ASSERT(propose_marshaller_);
    BOOST_ASSERT(exchange_marshaller_);
    BOOST_ASSERT(idmgr_);
    BOOST_ASSERT(key_marshaller_);

    /* Due to weird SECIO protobuf specification, we have to deal with a public
     * key in raw-bytes (marshalled) format. That is a known drawback.
     */
    auto public_key_res{
        key_marshaller_->marshal(idmgr_->getKeyPair().publicKey)};
    BOOST_ASSERT(public_key_res);
    propose_message_.pubkey.swap(public_key_res.value().key);
  }

  peer::ProtocolName Secio::getProtocolId() const {
    return kProtocolId;
  }

  void Secio::secureInbound(
      std::shared_ptr<connection::LayerConnection> inbound,
      SecurityAdaptor::SecConnCallbackFunc cb) {
    log_->info("securing inbound connection");
    auto dialer = std::make_shared<secio::Dialer>(inbound);
    sendProposeMessage(inbound, dialer, cb);
    receiveProposeMessage(inbound, dialer, cb);
  }

  void Secio::secureOutbound(
      std::shared_ptr<connection::LayerConnection> outbound,
      const peer::PeerId &p,
      SecurityAdaptor::SecConnCallbackFunc cb) {
    log_->info("securing outbound connection");
    auto dialer = std::make_shared<secio::Dialer>(outbound);
    sendProposeMessage(outbound, dialer, cb);
    receiveProposeMessage(outbound, dialer, cb);
  }

  void Secio::sendProposeMessage(
      const std::shared_ptr<connection::LayerConnection> &conn,
      const std::shared_ptr<secio::Dialer> &dialer,
      SecurityAdaptor::SecConnCallbackFunc cb) const {
    auto proto_propose{propose_marshaller_->handyToProto(propose_message_)};
    auto own_proposal_bytes = std::make_shared<std::vector<uint8_t>>();
    dialer->rw->write<secio::protobuf::Propose>(
        proto_propose,
        [self{shared_from_this()}, conn, cb{std::move(cb)}](
            auto &&res) mutable {
          if (res.has_error()) {
            self->closeConnection(conn, res.error());
            cb(res.error());
            return;
          }
        },
        own_proposal_bytes);
    dialer->storeLocalPeerProposalBytes(own_proposal_bytes);
    SL_TRACE(log_, "proposal sent");
  }

  void Secio::receiveProposeMessage(
      const std::shared_ptr<connection::LayerConnection> &conn,
      const std::shared_ptr<secio::Dialer> &dialer,
      SecurityAdaptor::SecConnCallbackFunc cb) const {
    auto remote_peer_proposal_bytes = std::make_shared<std::vector<uint8_t>>();
    dialer->rw->read<secio::protobuf::Propose>(
        [self{shared_from_this()},
         conn,
         dialer,
         cb{std::move(cb)},
         remote_peer_proposal_bytes](auto &&res) {
          if (res.has_error()) {
            self->closeConnection(conn, res.error());
            cb(res.error());
            return;
          }
          auto other_peer_proto_propose = std::move(res).value();
          auto remote_peer_propose{self->propose_marshaller_->protoToHandy(
              other_peer_proto_propose)};
          auto determine_algo_res =
              dialer->determineCommonAlgorithm(self->propose_message_,
                                               remote_peer_propose);
          if (determine_algo_res.has_error()) {
            self->closeConnection(conn, determine_algo_res.error());
            cb(determine_algo_res.error());
            return;
          }
          dialer->storeRemotePeerProposalBytes(remote_peer_proposal_bytes);
          SL_TRACE(self->log_, "remote peer proposal received");
          self->remote_peer_rand_.swap(remote_peer_propose.rand);
          self->sendExchangeMessage(conn, dialer, cb);
        },
        remote_peer_proposal_bytes);
  }

  void Secio::sendExchangeMessage(
      const std::shared_ptr<connection::LayerConnection> &conn,
      const std::shared_ptr<secio::Dialer> &dialer,
      SecurityAdaptor::SecConnCallbackFunc cb) const {
    const auto &&self{this};
    auto curve_res = dialer->chosenCurve();
    if (curve_res.has_error()) {
      self->closeConnection(conn, curve_res.error());
      cb(curve_res.error());
      return;
    }
    auto curve = std::move(curve_res).value();
    auto ephemeral_key_res = crypto_provider_->generateEphemeralKeyPair(curve);
    if (ephemeral_key_res.has_error()) {
      self->closeConnection(conn, ephemeral_key_res.error());
      cb(ephemeral_key_res.error());
      return;
    }
    auto ephemeral_key = std::move(ephemeral_key_res).value();
    dialer->storeEphemeralKeypair(ephemeral_key);
    auto local_corpus_res =
        dialer->getCorpus(true, ephemeral_key.ephemeral_public_key);
    if (local_corpus_res.has_error()) {
      self->closeConnection(conn, local_corpus_res.error());
      cb(local_corpus_res.error());
      return;
    }
    auto local_corpus = std::move(local_corpus_res).value();
    auto local_corpus_signature_res =
        crypto_provider_->sign(local_corpus, idmgr_->getKeyPair().privateKey);
    if (local_corpus_signature_res.has_error()) {
      self->closeConnection(conn, local_corpus_signature_res.error());
      cb(local_corpus_signature_res.error());
      return;
    }
    auto local_corpus_signature = std::move(local_corpus_signature_res).value();
    secio::ExchangeMessage local_exchange{
        .epubkey = ephemeral_key.ephemeral_public_key,
        .signature = std::move(local_corpus_signature)};
    auto proto_exchange{exchange_marshaller_->handyToProto(local_exchange)};
    dialer->rw->write<secio::protobuf::Exchange>(
        proto_exchange,
        [self{shared_from_this()}, conn, dialer, cb{std::move(cb)}](
            auto &&res) {
          if (res.has_error()) {
            self->closeConnection(conn, res.error());
            cb(res.error());
            return;
          }
          SL_TRACE(self->log_, "exchange message sent");
          self->receiveExchangeMessage(conn, dialer, cb);
        });
  }

  void Secio::receiveExchangeMessage(
      const std::shared_ptr<connection::LayerConnection> &conn,
      const std::shared_ptr<secio::Dialer> &dialer,
      SecurityAdaptor::SecConnCallbackFunc cb) const {
    dialer->rw->read<secio::protobuf::Exchange>(
        [self{shared_from_this()}, conn, dialer, cb{std::move(cb)}](
            auto &&res) {
          if (res.has_error()) {
            self->closeConnection(conn, res.error());
            cb(res.error());
            return;
          }
          auto remote_proto_exchange = std::move(res).value();
          auto remote_exchange{
              self->exchange_marshaller_->protoToHandy(remote_proto_exchange)};
          SL_TRACE(self->log_, "remote exchange message received");
          auto remote_corpus_res =
              dialer->getCorpus(false, remote_exchange.epubkey);
          if (remote_corpus_res.has_error()) {
            self->closeConnection(conn, remote_corpus_res.error());
            cb(remote_corpus_res.error());
            return;
          }
          auto remote_corpus = std::move(remote_corpus_res).value();

          auto remote_key_res =
              dialer->remotePublicKey(self->key_marshaller_,
                                      self->propose_marshaller_);
          if (remote_key_res.has_error()) {
            self->closeConnection(conn, remote_key_res.error());
            cb(remote_key_res.error());
            return;
          }
          auto remote_key = std::move(remote_key_res).value();
          auto verify_res_res = self->crypto_provider_->verify(
              remote_corpus, remote_exchange.signature, remote_key);
          if (verify_res_res.has_error()) {
            self->closeConnection(conn, verify_res_res.error());
            cb(verify_res_res.error());
            return;
          }
          auto verify_res = std::move(verify_res_res).value();
          if (!verify_res) {
            const auto error{Error::REMOTE_PEER_SIGNATURE_IS_INVALID};
            self->closeConnection(conn, error);
            cb(error);
            return;
          }

          auto shared_secret_res =
              dialer->generateSharedSecret(remote_exchange.epubkey);
          if (shared_secret_res.has_error()) {
            self->closeConnection(conn, shared_secret_res.error());
            cb(shared_secret_res.error());
            return;
          }
          auto shared_secret = std::move(shared_secret_res).value();
          auto chosen_cipher_res = dialer->chosenCipher();
          if (chosen_cipher_res.has_error()) {
            self->closeConnection(conn, chosen_cipher_res.error());
            cb(chosen_cipher_res.error());
            return;
          }
          auto chosen_cipher = std::move(chosen_cipher_res).value();
          auto chosen_hash_res = dialer->chosenHash();
          if (chosen_hash_res.has_error()) {
            self->closeConnection(conn, chosen_hash_res.error());
            cb(chosen_hash_res.error());
            return;
          }
          auto chosen_hash = std::move(chosen_hash_res).value();
          auto stretched_keys_res = self->crypto_provider_->stretchKey(
              chosen_cipher, chosen_hash, shared_secret);
          if (stretched_keys_res.has_error()) {
            self->closeConnection(conn, stretched_keys_res.error());
            cb(stretched_keys_res.error());
            return;
          }
          auto stretched_keys = std::move(stretched_keys_res).value();
          dialer->storeStretchedKeys(std::move(stretched_keys));

          auto remote_pubkey_res =
              dialer->remotePublicKey(self->key_marshaller_,
                                      self->propose_marshaller_);
          if (remote_pubkey_res.has_error()) {
            self->closeConnection(conn, remote_pubkey_res.error());
            cb(remote_pubkey_res.error());
            return;
          }
          auto remote_pubkey = std::move(remote_pubkey_res).value();
          auto local_stretched_key_res = dialer->localStretchedKey();
          if (local_stretched_key_res.has_error()) {
            self->closeConnection(conn, local_stretched_key_res.error());
            cb(local_stretched_key_res.error());
            return;
          }
          auto local_stretched_key = std::move(local_stretched_key_res).value();
          auto remote_stretched_key_res = dialer->remoteStretchedKey();
          if (remote_stretched_key_res.has_error()) {
            self->closeConnection(conn, remote_stretched_key_res.error());
            cb(remote_stretched_key_res.error());
            return;
          }
          auto remote_stretched_key =
              std::move(remote_stretched_key_res).value();

          auto secio_conn = std::make_shared<connection::SecioConnection>(
              conn,
              self->hmac_provider_,
              self->key_marshaller_,
              self->idmgr_->getKeyPair().publicKey,
              remote_pubkey,
              chosen_hash,
              chosen_cipher,
              local_stretched_key,
              remote_stretched_key);
          auto secio_conn_init_res = secio_conn->init();
          if (secio_conn_init_res.has_error()) {
            self->closeConnection(conn, secio_conn_init_res.error());
            cb(secio_conn_init_res.error());
            return;
          }
          write(secio_conn,
                self->remote_peer_rand_,
                [self, conn, cb, secio_conn](outcome::result<void> write_res) {
                  if (write_res.has_error()) {
                    self->closeConnection(conn, write_res.error());
                    cb(write_res.error());
                    return;
                  }
                  const auto kToRead{self->propose_message_.rand.size()};
                  auto buffer = std::make_shared<Bytes>(kToRead);
                  read(secio_conn,
                       *buffer,
                       [self, cb, conn, secio_conn, buffer](
                           outcome::result<void> read_res) {
                         if (read_res.has_error()) {
                           self->closeConnection(conn, read_res.error());
                           cb(read_res.error());
                           return;
                         }
                         if (*buffer != self->propose_message_.rand) {
                           return cb(Error::INITIAL_PACKET_VERIFICATION_FAILED);
                         }
                         SL_TRACE(self->log_, "connection initialized");
                         cb(secio_conn);
                       });
                });
        });
  }

  void Secio::closeConnection(
      const std::shared_ptr<libp2p::connection::LayerConnection> &conn,
      const std::error_code &err) const {
    log_->error("error happened, closing connection: {}", err);
    if (auto close_res = conn->close(); !close_res) {
      log_->error("connection close attempt ended with error: {}",
                  close_res.error());
    }
  }

}  // namespace libp2p::security
