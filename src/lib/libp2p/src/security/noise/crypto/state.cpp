/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstring>
#include <limits>
#include <sstream>

#include <libp2p/security/noise/crypto/state.hpp>

OUTCOME_CPP_DEFINE_CATEGORY(libp2p::security::noise, Error, e) {
  using E = libp2p::security::noise::Error;
  switch (e) {
    case E::INTERNAL_ERROR:
      return "Undefined behavior caused an internal error";
    case E::WRONG_KEY32_SIZE:
      return "Key size does not match to expected (32 bytes)";
    case E::EMPTY_HANDSHAKE_NAME:
      return "Handshake name cannot be empty";
    case E::WRONG_PRESHARED_KEY_SIZE:
      return "Noise spec mandates 256-bit preshared keys";
    case E::NOT_INITIALIZED:
      return "Handshake state is not initialized";
    case E::UNEXPECTED_WRITE_CALL:
      return "Unexpected call to writeMessage should be readMessage";
    case E::UNEXPECTED_READ_CALL:
      return "Unexpected call to readMessage should be writeMessage";
    case E::NO_HANDSHAKE_MESSAGE:
      return "No handshake messages left";
    case E::MESSAGE_TOO_LONG:
      return "Message is too long";
    case E::MESSAGE_TOO_SHORT:
      return "Message is too short";
    case E::NO_PUBLIC_KEY:
      return "Invalid state, no public key";
    case E::REMOTE_KEY_ALREADY_SET:
      return "Invalid state, remote shared public key is already set";
  }
  return "unknown error";
}

namespace libp2p::security::noise {

  outcome::result<Key32> bytesToKey32(BytesIn key) {
    Key32 result;
    if (static_cast<size_t>(key.size()) != result.size()) {
      return Error::WRONG_KEY32_SIZE;
    }
    std::copy_n(key.begin(), result.size(), result.begin());
    return result;
  }

  // Cipher State

  CipherState::CipherState(std::shared_ptr<CipherSuite> cipher_suite, Key32 key)
      : cipher_suite_{std::move(cipher_suite)},
        key_{key},
        cipher_{cipher_suite_->cipher(key_)},
        nonce_{0} {}

  outcome::result<Bytes> CipherState::encrypt(BytesIn precompiled_out,
                                              BytesIn plaintext,
                                              BytesIn aad) {
    auto enc_res = cipher_->encrypt(precompiled_out, nonce_, plaintext, aad);
    ++nonce_;
    return enc_res;
  }

  outcome::result<Bytes> CipherState::decrypt(BytesIn precompiled_out,
                                              BytesIn ciphertext,
                                              BytesIn aad) {
    auto dec_res = cipher_->decrypt(precompiled_out, nonce_, ciphertext, aad);
    ++nonce_;
    return dec_res;
  }

  outcome::result<void> CipherState::rekey() {
    Key32 zeroed;
    memset(zeroed.data(), 0u, zeroed.size());
    Bytes empty;
    auto out_res_res =
        cipher_->encrypt({}, std::numeric_limits<uint64_t>::max(), zeroed, empty);
    if (!out_res_res) {
      return out_res_res.error();
    }
    auto out_res = std::move(out_res_res).value();
    std::copy_n(out_res.begin(), key_.size(), key_.begin());
    cipher_ = cipher_suite_->cipher(key_);
    return outcome::success();
  }

  std::shared_ptr<CipherSuite> CipherState::cipherSuite() const {
    return cipher_suite_;
  }

  // Symmetric State

  SymmetricState::SymmetricState(std::shared_ptr<CipherSuite> cipher_suite)
      : CipherState(std::move(cipher_suite), Key32{}) {}

  outcome::result<void> SymmetricState::initializeSymmetric(
      BytesIn handshake_name) {
    if (handshake_name.empty()) {
      return Error::EMPTY_HANDSHAKE_NAME;
    }
    auto hasher = cipher_suite_->hash();
    // static cast is 100% safe here
    if (static_cast<uint64_t>(handshake_name.size()) <= hasher->digestSize()) {
      hash_.resize(hasher->digestSize());
      std::copy_n(handshake_name.begin(), handshake_name.size(), hash_.begin());
    } else {
      auto hasher_write_res = hasher->write(handshake_name);
      if (!hasher_write_res) {
        return hasher_write_res.error();
      }
      auto hash_res_res = hasher->digest();
      if (!hash_res_res) {
        return hash_res_res.error();
      }
      auto hash_res = std::move(hash_res_res).value();
      hash_ = std::move(hash_res);
    }
    chaining_key_ = hash_;
    return outcome::success();
  }

  outcome::result<void> SymmetricState::mixKey(BytesIn dh_output) {
    nonce_ = 0;
    has_key_ = true;
    auto hkdf_res_res =
        hkdf(cipher_suite_->hash()->hashType(), 2, chaining_key_, dh_output);
    if (!hkdf_res_res) {
      return hkdf_res_res.error();
    }
    auto hkdf_res = std::move(hkdf_res_res).value();
    chaining_key_ = std::move(hkdf_res.one);
    auto hash_key_res = bytesToKey32(hkdf_res.two);
    if (!hash_key_res) {
      return hash_key_res.error();
    }
    auto hash_key = std::move(hash_key_res).value();
    key_ = hash_key;
    cipher_ = cipher_suite_->cipher(key_);
    return outcome::success();
  }

  outcome::result<void> SymmetricState::mixHash(BytesIn data) {
    auto hasher = cipher_suite_->hash();
    auto hasher_write_hash_res = hasher->write(hash_);
    if (!hasher_write_hash_res) {
      return hasher_write_hash_res.error();
    }
    auto hasher_write_data_res = hasher->write(data);
    if (!hasher_write_data_res) {
      return hasher_write_data_res.error();
    }
    auto hash_res_res = hasher->digest();
    if (!hash_res_res) {
      return hash_res_res.error();
    }
    auto hash_res = std::move(hash_res_res).value();
    hash_ = hash_res;
    return outcome::success();
  }

  outcome::result<void> SymmetricState::mixKeyAndHash(BytesIn data) {
    auto hkdf_res_res =
        hkdf(cipher_suite_->hash()->hashType(), 3, chaining_key_, data);
    if (!hkdf_res_res) {
      return hkdf_res_res.error();
    }
    auto hkdf_res = std::move(hkdf_res_res).value();
    chaining_key_ = hkdf_res.one;  // ck
    auto mix_hash_res = mixHash(hkdf_res.two);
    if (!mix_hash_res) {
      return mix_hash_res.as_failure();
    }
    auto hash_key_res = bytesToKey32(hkdf_res.three);
    if (!hash_key_res) {
      return hash_key_res.as_failure();
    }
    auto hash_key = std::move(hash_key_res).value();
    key_ = hash_key;
    cipher_ = cipher_suite_->cipher(key_);
    nonce_ = 0;
    has_key_ = true;
    return outcome::success();
  }

  outcome::result<Bytes> SymmetricState::encryptAndHash(BytesIn precompiled_out,
                                                        BytesIn plaintext) {
    if (not has_key_) {
      Bytes result(precompiled_out.size() + plaintext.size());
      auto mix_hash_res = mixHash(plaintext);
      if (!mix_hash_res) {
        return mix_hash_res.as_failure();
      }
      std::copy_n(
          precompiled_out.begin(), precompiled_out.size(), result.begin());
      std::copy_n(plaintext.begin(),
                  plaintext.size(),
                  result.begin() + precompiled_out.size());
      return result;
    }
    auto ciphertext_res = encrypt(precompiled_out, plaintext, hash_);
    if (!ciphertext_res) {
      return ciphertext_res.error();
    }
    auto ciphertext = std::move(ciphertext_res).value();
    auto ct_size = ciphertext.size();
    auto po_size = precompiled_out.size();
    if (po_size > ct_size) {
      // the same gonna happen in go-libp2p, moreover it is not anyhow checked
      // and handled there!
      return Error::INTERNAL_ERROR;
    }
    Bytes seed(ct_size - po_size);
    std::copy_n(ciphertext.begin() + po_size, seed.size(), seed.begin());
    auto mix_hash_res = mixHash(seed);
    if (!mix_hash_res) {
      return mix_hash_res.as_failure();
    }
    return std::move(ciphertext);
  }

  outcome::result<Bytes> SymmetricState::decryptAndHash(BytesIn precompiled_out,
                                                        BytesIn ciphertext) {
    if (not has_key_) {
      Bytes result(precompiled_out.size() + ciphertext.size());
      auto mix_hash_res = mixHash(ciphertext);
      if (!mix_hash_res) {
        return mix_hash_res.as_failure();
      }
      std::copy_n(
          precompiled_out.begin(), precompiled_out.size(), result.begin());
      std::copy_n(ciphertext.begin(),
                  ciphertext.size(),
                  result.begin() + precompiled_out.size());
      return result;
    }
    auto plaintext_res = decrypt(precompiled_out, ciphertext, hash_);
    if (!plaintext_res) {
      return plaintext_res.error();
    }
    auto plaintext = std::move(plaintext_res).value();
    auto mix_hash_res = mixHash(ciphertext);
    if (!mix_hash_res) {
      return mix_hash_res.as_failure();
    }
    return std::move(plaintext);
  }

  outcome::result<SymmetricState::CSPair> SymmetricState::split() {
    auto hkdf_res_res =
        hkdf(cipher_suite_->hash()->hashType(), 2, chaining_key_, {});
    if (!hkdf_res_res) {
      return hkdf_res_res.error();
    }
    auto hkdf_res = std::move(hkdf_res_res).value();
    auto hash_key1_res = bytesToKey32(hkdf_res.one);
    if (!hash_key1_res) {
      return hash_key1_res.as_failure();
    }
    auto hash_key1 = std::move(hash_key1_res).value();
    auto hash_key2_res = bytesToKey32(hkdf_res.two);
    if (!hash_key2_res) {
      return hash_key2_res.as_failure();
    }
    auto hash_key2 = std::move(hash_key2_res).value();
    auto first_state = std::make_shared<CipherState>(cipher_suite_, hash_key1);
    auto second_state = std::make_shared<CipherState>(cipher_suite_, hash_key2);
    first_state->cipher_ = cipher_suite_->cipher(first_state->key_);
    second_state->cipher_ = cipher_suite_->cipher(second_state->key_);
    return std::make_pair(std::move(first_state), std::move(second_state));
  }

  void SymmetricState::checkpoint() {
    prev_chaining_key_ = chaining_key_;
    prev_hash_ = hash_;
  }

  void SymmetricState::rollback() {
    chaining_key_ = prev_chaining_key_;
    hash_ = prev_hash_;
  }

  Bytes SymmetricState::hash() const {
    return hash_;
  }

  bool SymmetricState::hasKey() const {
    return has_key_;
  }

  // Handshake state config

  HandshakeStateConfig::HandshakeStateConfig(
      std::shared_ptr<CipherSuite> cipher_suite,
      HandshakePattern pattern,
      bool is_initiator,
      DHKey local_static_keypair)
      : cipher_suite_{std::move(cipher_suite)},
        pattern_{std::move(pattern)},
        is_initiator_{is_initiator},
        local_static_keypair_{std::move(local_static_keypair)} {}

  HandshakeStateConfig &HandshakeStateConfig::setPrologue(BytesIn prologue) {
    Bytes data(prologue.begin(), prologue.end());
    prologue_ = std::move(data);
    return *this;
  }

  HandshakeStateConfig &HandshakeStateConfig::setPresharedKey(BytesIn key,
                                                              int placement) {
    Bytes data(key.begin(), key.end());
    preshared_key_ = std::move(data);
    preshared_key_placement_ = placement;
    return *this;
  }

  HandshakeStateConfig &HandshakeStateConfig::setLocalEphemeralKeypair(
      DHKey keypair) {
    local_ephemeral_keypair_ = std::move(keypair);
    return *this;
  }

  HandshakeStateConfig &HandshakeStateConfig::setRemoteStaticPubkey(
      BytesIn key) {
    Bytes data(key.begin(), key.end());
    remote_static_pubkey_ = std::move(data);
    return *this;
  }

  HandshakeStateConfig &HandshakeStateConfig::setRemoteEphemeralPubkey(
      BytesIn key) {
    Bytes data(key.begin(), key.end());
    remote_ephemeral_pubkey_ = std::move(data);
    return *this;
  }

  // Handshake state

  outcome::result<void> HandshakeState::init(HandshakeStateConfig config) {
    local_static_kp_ = std::move(config.local_static_keypair_);
    message_patterns_ = std::move(config.pattern_.messages);
    is_initiator_ = config.is_initiator_;
    should_write_ = is_initiator_;
    message_idx_ = 0;
    if (config.local_ephemeral_keypair_) {
      local_ephemeral_kp_ = std::move(config.local_ephemeral_keypair_.value());
    }
    if (config.remote_static_pubkey_) {
      remote_static_pubkey_ = std::move(config.remote_static_pubkey_.value());
    }
    if (config.remote_ephemeral_pubkey_) {
      remote_ephemeral_pubkey_ =
          std::move(config.remote_ephemeral_pubkey_.value());
    }
    if (config.preshared_key_) {
      preshared_key_ = std::move(config.preshared_key_.value());
    }
    symmetric_state_ =
        std::make_unique<SymmetricState>(std::move(config.cipher_suite_));
    int preshared_key_placement{0};
    if (config.preshared_key_placement_) {
      preshared_key_placement = config.preshared_key_placement_.value();
    }
    std::string psk_modifier;
    if (not preshared_key_.empty()) {
      if (32 != preshared_key_.size()) {
        return Error::WRONG_PRESHARED_KEY_SIZE;
      }
      psk_modifier = "psk" + std::to_string(preshared_key_placement);
      if (0 == preshared_key_placement) {
        message_patterns_[0].insert(message_patterns_[0].begin(),
                                    MessagePattern::PSK);
      } else {
        message_patterns_[preshared_key_placement - 1].push_back(
            MessagePattern::PSK);
      }
    }
    std::stringstream ss;
    ss << "Noise_" << config.pattern_.name << psk_modifier << "_"
       << symmetric_state_->cipherSuite()->name();
    auto handshake_name_str = ss.str();
    Bytes handshake_name_bytes(handshake_name_str.begin(),
                               handshake_name_str.end());
    auto init_symmetric_res =
        symmetric_state_->initializeSymmetric(handshake_name_bytes);
    if (!init_symmetric_res) {
      return init_symmetric_res.as_failure();
    }
    Bytes prologue;
    if (config.prologue_) {
      prologue = std::move(config.prologue_.value());
    }
    auto mix_hash_prologue_res = symmetric_state_->mixHash(prologue);
    if (!mix_hash_prologue_res) {
      return mix_hash_prologue_res.as_failure();
    }
    for (const auto &m : config.pattern_.initiatorPreMessages) {
      if (is_initiator_ and MessagePattern::S == m) {
        auto mix_hash_res = symmetric_state_->mixHash(local_static_kp_.pub);
        if (!mix_hash_res) {
          return mix_hash_res.as_failure();
        }
      } else if (is_initiator_ and MessagePattern::E == m) {
        auto mix_hash_res = symmetric_state_->mixHash(local_ephemeral_kp_.pub);
        if (!mix_hash_res) {
          return mix_hash_res.as_failure();
        }
      } else if (not is_initiator_ and MessagePattern::S == m) {
        auto mix_hash_res = symmetric_state_->mixHash(remote_static_pubkey_);
        if (!mix_hash_res) {
          return mix_hash_res.as_failure();
        }
      } else if (not is_initiator_ and MessagePattern::E == m) {
        auto mix_hash_res =
            symmetric_state_->mixHash(remote_ephemeral_pubkey_);
        if (!mix_hash_res) {
          return mix_hash_res.as_failure();
        }
      }
    }
    for (const auto &m : config.pattern_.responderPreMessages) {
      if (not is_initiator_ and MessagePattern::S == m) {
        auto mix_hash_res = symmetric_state_->mixHash(local_static_kp_.pub);
        if (!mix_hash_res) {
          return mix_hash_res.as_failure();
        }
      } else if (not is_initiator_ and MessagePattern::E == m) {
        auto mix_hash_res = symmetric_state_->mixHash(local_ephemeral_kp_.pub);
        if (!mix_hash_res) {
          return mix_hash_res.as_failure();
        }
      } else if (is_initiator_ and MessagePattern::S == m) {
        auto mix_hash_res = symmetric_state_->mixHash(remote_static_pubkey_);
        if (!mix_hash_res) {
          return mix_hash_res.as_failure();
        }
      } else if (is_initiator_ and MessagePattern::E == m) {
        auto mix_hash_res =
            symmetric_state_->mixHash(remote_ephemeral_pubkey_);
        if (!mix_hash_res) {
          return mix_hash_res.as_failure();
        }
      }
    }
    is_initialized_ = true;
    return outcome::success();
  }

  outcome::result<HandshakeState::MessagingResult> HandshakeState::writeMessage(
      BytesIn precompiled_out, BytesIn payload) {
    auto is_initialized_res = isInitialized();
    if (!is_initialized_res) {
      return is_initialized_res.as_failure();
    }
    if (not should_write_) {
      return Error::UNEXPECTED_WRITE_CALL;
    }
    if (message_idx_ > static_cast<int64_t>(message_patterns_.size()) - 1) {
      return Error::NO_HANDSHAKE_MESSAGE;
    }
    if (payload.size() > static_cast<int64_t>(kMaxMsgLen)) {
      return Error::MESSAGE_TOO_LONG;
    }
    auto out = spanToVec(precompiled_out);
    for (const auto &message : message_patterns_[message_idx_]) {
      outcome::result<void> err = outcome::success();
      switch (message) {
        case MessagePattern::E:
          err = writeMessageE(out);
          break;
        case MessagePattern::S:
          err = writeMessageS(out);
          break;
        case MessagePattern::DHEE:
          err = writeMessageDHEE(out);
          break;
        case MessagePattern::DHES:
          err = writeMessageDHES(out);
          break;
        case MessagePattern::DHSE:
          err = writeMessageDHSE(out);
          break;
        case MessagePattern::DHSS:
          err = writeMessageDHSS(out);
          break;
        case MessagePattern::PSK:
          err = writeMessagePSK();
          break;
      }
      if (!err) {
        return err.as_failure();
      }
    }
    should_write_ = false;
    ++message_idx_;
    auto output_res = symmetric_state_->encryptAndHash(out, payload);
    if (!output_res) {
      return output_res.error();
    }
    auto output = std::move(output_res).value();
    HandshakeState::MessagingResult result;
    result.data.swap(output);
    if (message_idx_ >= static_cast<int64_t>(message_patterns_.size())) {
      auto cs_pair_res = symmetric_state_->split();
      if (!cs_pair_res) {
        return cs_pair_res.as_failure();
      }
      auto cs_pair = std::move(cs_pair_res).value();
      result.cs1 = cs_pair.first;
      result.cs2 = cs_pair.second;
    }
    return result;
  }

  outcome::result<void> HandshakeState::writeMessageE(Bytes &out) {
    auto ephemeral_kp_res = symmetric_state_->cipherSuite()->generate();
    if (!ephemeral_kp_res) {
      return ephemeral_kp_res.as_failure();
    }
    auto ephemeral_kp = std::move(ephemeral_kp_res).value();
    local_ephemeral_kp_ = std::move(ephemeral_kp);
    out.insert(out.end(),
               local_ephemeral_kp_.pub.begin(),
               local_ephemeral_kp_.pub.end());
    auto mix_hash_res = symmetric_state_->mixHash(local_ephemeral_kp_.pub);
    if (!mix_hash_res) {
      return mix_hash_res.as_failure();
    }
    if (not preshared_key_.empty()) {
      auto mix_key_res = symmetric_state_->mixKey(local_ephemeral_kp_.pub);
      if (!mix_key_res) {
        return mix_key_res.as_failure();
      }
    }
    return outcome::success();
  }

  outcome::result<void> HandshakeState::writeMessageS(Bytes &out) {
    if (local_static_kp_.pub.empty()) {
      return Error::NO_PUBLIC_KEY;
    }
    auto output_res = symmetric_state_->encryptAndHash(out, local_static_kp_.pub);
    if (!output_res) {
      return output_res.error();
    }
    auto output = std::move(output_res).value();
    out.swap(output);
    return outcome::success();
  }

  outcome::result<void> HandshakeState::writeKem(Bytes &out,
                                                 const Bytes &remote_pk) {
    auto enc_res = symmetric_state_->cipherSuite()->encapsulate(remote_pk);
    if (!enc_res) {
      return enc_res.error();
    }
    auto enc = std::move(enc_res).value();
    out.insert(out.end(), enc.ciphertext.begin(), enc.ciphertext.end());
    return symmetric_state_->mixKey(enc.shared_secret);
  }

  outcome::result<void> HandshakeState::readKem(Bytes &message,
                                                const Bytes &local_sk) {
    const auto ct_len = symmetric_state_->cipherSuite()->ciphertextSize();
    if (static_cast<int64_t>(message.size()) < ct_len) {
      return Error::MESSAGE_TOO_SHORT;
    }
    Bytes ciphertext{message.begin(), message.begin() + ct_len};
    auto ss_res =
        symmetric_state_->cipherSuite()->decapsulate(local_sk, ciphertext);
    if (!ss_res) {
      return ss_res.error();
    }
    Bytes(message.begin() + ct_len, message.end()).swap(message);
    return symmetric_state_->mixKey(ss_res.value());
  }

  outcome::result<void> HandshakeState::writeMessageDHEE(Bytes &out) {
    return writeKem(out, remote_ephemeral_pubkey_);
  }

  outcome::result<void> HandshakeState::writeMessageDHES(Bytes &out) {
    if (is_initiator_) {
      return writeKem(out, remote_static_pubkey_);
    }
    return writeKem(out, remote_ephemeral_pubkey_);
  }

  outcome::result<void> HandshakeState::writeMessageDHSE(Bytes &out) {
    if (is_initiator_) {
      return writeKem(out, remote_ephemeral_pubkey_);
    }
    return writeKem(out, remote_static_pubkey_);
  }

  outcome::result<void> HandshakeState::writeMessageDHSS(Bytes &out) {
    return writeKem(out, remote_static_pubkey_);
  }

  outcome::result<void> HandshakeState::writeMessagePSK() {
    return symmetric_state_->mixKeyAndHash(preshared_key_);
  }

  outcome::result<HandshakeState::MessagingResult> HandshakeState::readMessage(
      BytesIn precompiled_out, BytesIn message) {
    auto is_initialized_res = isInitialized();
    if (!is_initialized_res) {
      return is_initialized_res.as_failure();
    }
    if (should_write_) {
      return Error::UNEXPECTED_READ_CALL;
    }
    if (message_idx_ > static_cast<int64_t>(message_patterns_.size()) - 1) {
      return Error::NO_HANDSHAKE_MESSAGE;
    }
    symmetric_state_->checkpoint();
    auto msg = spanToVec(message);
    for (const auto &pattern : message_patterns_[message_idx_]) {
      outcome::result<void> err = outcome::success();
      switch (pattern) {
        case MessagePattern::E:
          err = readMessageE(msg);
          break;
        case MessagePattern::S:
          err = readMessageS(msg);
          break;
        case MessagePattern::DHEE:
          err = readMessageDHEE(msg);
          break;
        case MessagePattern::DHES:
          err = readMessageDHES(msg);
          break;
        case MessagePattern::DHSE:
          err = readMessageDHSE(msg);
          break;
        case MessagePattern::DHSS:
          err = readMessageDHSS(msg);
          break;
        case MessagePattern::PSK:
          err = readMessagePSK();
          break;
      }
      if (!err) {
        return err.as_failure();
      }
    }
    auto decrypted = symmetric_state_->decryptAndHash(precompiled_out, msg);
    if (decrypted.has_error()) {
      symmetric_state_->rollback();
      return decrypted.error();
    }
    should_write_ = true;
    ++message_idx_;
    HandshakeState::MessagingResult result;
    result.data.swap(decrypted.value());
    if (message_idx_ >= static_cast<int64_t>(message_patterns_.size())) {
      auto cs_pair_res = symmetric_state_->split();
      if (!cs_pair_res) {
        return cs_pair_res.as_failure();
      }
      auto cs_pair = std::move(cs_pair_res).value();
      result.cs1 = cs_pair.first;
      result.cs2 = cs_pair.second;
    }
    return result;
  }

  outcome::result<void> HandshakeState::readMessageE(Bytes &message) {
    auto expected = symmetric_state_->cipherSuite()->dhSize();
    if (static_cast<int64_t>(message.size()) < expected) {
      return Error::MESSAGE_TOO_SHORT;
    }
    remote_ephemeral_pubkey_ =
        Bytes{message.begin(), message.begin() + expected};
    auto mix_hash_res = symmetric_state_->mixHash(remote_ephemeral_pubkey_);
    if (!mix_hash_res) {
      return mix_hash_res.as_failure();
    }
    if (not preshared_key_.empty()) {
      auto mix_key_res = symmetric_state_->mixKey(remote_ephemeral_pubkey_);
      if (!mix_key_res) {
        return mix_key_res.as_failure();
      }
    }
    Bytes(message.begin() + expected, message.end()).swap(message);
    return outcome::success();
  }

  outcome::result<void> HandshakeState::readMessageS(Bytes &message) {
    auto expected = symmetric_state_->cipherSuite()->dhSize();
    if (symmetric_state_->hasKey()) {
      expected += 16;
    }
    if (static_cast<int64_t>(message.size()) < expected) {
      return Error::MESSAGE_TOO_SHORT;
    }
    if (not remote_static_pubkey_.empty()) {
      return Error::REMOTE_KEY_ALREADY_SET;
    }
    auto decrypted = symmetric_state_->decryptAndHash(
        {}, std::span(message.data(), expected));
    if (decrypted.has_error()) {
      symmetric_state_->rollback();
      return decrypted.error();
    }
    remote_static_pubkey_ = std::move(decrypted.value());
    Bytes(message.begin() + expected, message.end()).swap(message);
    return outcome::success();
  }

  outcome::result<void> HandshakeState::readMessageDHEE(Bytes &message) {
    return readKem(message, local_ephemeral_kp_.priv);
  }

  outcome::result<void> HandshakeState::readMessageDHES(Bytes &message) {
    if (is_initiator_) {
      return readKem(message, local_ephemeral_kp_.priv);
    }
    return readKem(message, local_static_kp_.priv);
  }

  outcome::result<void> HandshakeState::readMessageDHSE(Bytes &message) {
    if (is_initiator_) {
      return readKem(message, local_static_kp_.priv);
    }
    return readKem(message, local_ephemeral_kp_.priv);
  }

  outcome::result<void> HandshakeState::readMessageDHSS(Bytes &message) {
    return readKem(message, local_static_kp_.priv);
  }

  outcome::result<void> HandshakeState::readMessagePSK() {
    return symmetric_state_->mixKeyAndHash(preshared_key_);
  }

  outcome::result<Bytes> HandshakeState::channelBinding() const {
    auto is_initialized_res = isInitialized();
    if (!is_initialized_res) {
      return is_initialized_res.as_failure();
    }
    return symmetric_state_->hash();
  }

  outcome::result<Bytes> HandshakeState::remotePeerStaticPubkey() const {
    auto is_initialized_res = isInitialized();
    if (!is_initialized_res) {
      return is_initialized_res.as_failure();
    }
    return remote_static_pubkey_;
  }

  outcome::result<Bytes> HandshakeState::remotePeerEphemeralPubkey() const {
    auto is_initialized_res = isInitialized();
    if (!is_initialized_res) {
      return is_initialized_res.as_failure();
    }
    return remote_ephemeral_pubkey_;
  }

  outcome::result<DHKey> HandshakeState::localPeerEphemeralKey() const {
    auto is_initialized_res = isInitialized();
    if (!is_initialized_res) {
      return is_initialized_res.as_failure();
    }
    return local_ephemeral_kp_;
  }

  outcome::result<int> HandshakeState::messageIndex() const {
    auto is_initialized_res = isInitialized();
    if (!is_initialized_res) {
      return is_initialized_res.as_failure();
    }
    return message_idx_;
  }

  outcome::result<void> HandshakeState::isInitialized() const {
    if (is_initialized_) {
      return outcome::success();
    }
    return Error::NOT_INITIALIZED;
  }

}  // namespace libp2p::security::noise
