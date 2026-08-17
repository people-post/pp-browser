/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/security/noise/crypto/cipher_suite.hpp>

#include <sstream>

namespace libp2p::security::noise {

  CipherSuiteImpl::CipherSuiteImpl(std::shared_ptr<DiffieHellman> dh,
                                   std::shared_ptr<NamedHasher> hash,
                                   std::shared_ptr<NamedAEADCipher> cipher)
      : dh_{std::move(dh)},
        hash_{std::move(hash)},
        cipher_{std::move(cipher)} {}

  outcome::result<DHKey> CipherSuiteImpl::generate() {
    return dh_->generate();
  }

  outcome::result<Bytes> CipherSuiteImpl::dh(const Bytes &private_key,
                                             const Bytes &public_key) {
    return dh_->dh(private_key, public_key);
  }

  outcome::result<KemEncapsulateResult> CipherSuiteImpl::encapsulate(
      const Bytes &public_key) {
    return dh_->encapsulate(public_key);
  }

  outcome::result<Bytes> CipherSuiteImpl::decapsulate(
      const Bytes &private_key, const Bytes &ciphertext) {
    return dh_->decapsulate(private_key, ciphertext);
  }

  int CipherSuiteImpl::dhSize() const {
    return dh_->dhSize();
  }

  int CipherSuiteImpl::ciphertextSize() const {
    return dh_->ciphertextSize();
  }

  std::string CipherSuiteImpl::dhName() const {
    return dh_->dhName();
  }

  std::shared_ptr<crypto::Hasher> CipherSuiteImpl::hash() {
    return hash_->hash();
  }

  std::string CipherSuiteImpl::hashName() const {
    return hash_->hashName();
  }

  std::shared_ptr<AEADCipher> CipherSuiteImpl::cipher(Key32 key) {
    return cipher_->cipher(key);
  }

  std::string CipherSuiteImpl::cipherName() const {
    return cipher_->cipherName();
  }

  std::string CipherSuiteImpl::name() {
    std::stringstream s;
    s << dhName() << "_" << cipherName() << "_" << hashName();
    return s.str();
  }

}  // namespace libp2p::security::noise
