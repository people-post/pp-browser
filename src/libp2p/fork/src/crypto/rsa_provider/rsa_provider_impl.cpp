/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/crypto/rsa_provider/rsa_provider_impl.hpp>

#include <memory>

#include <openssl/err.h>
#include <openssl/mem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <libp2p/common/final_action.hpp>
#include <libp2p/crypto/sha/sha256.hpp>

using Hash256 = libp2p::common::Hash256;
using libp2p::common::FinalAction;

namespace {
  /// encode cryptographic key in `ASN.1 DER` format
  template <class KeyStructure, class Function>
  outcome::result<std::vector<uint8_t>> encodeKeyDer(KeyStructure *ks,
                                                     Function *function) {
    unsigned char *buffer = nullptr;
    FinalAction cleanup([pptr = &buffer] {
      if (*pptr != nullptr) {
        OPENSSL_free(*pptr);
      }
    });

    int length = function(ks, &buffer);
    if (length < 0) {
      return libp2p::crypto::KeyGeneratorError::KEY_GENERATION_FAILED;
    }

    auto span = std::span(buffer, length);
    return std::vector<uint8_t>{span.begin(), span.end()};
  }
}  // namespace

namespace libp2p::crypto::rsa {

  /**
   * according to libp2p specification:
   *  https://github.com/libp2p/specs/blob/master/peer-ids/peer-ids.md#how-keys-are-encoded-and-messages-signed
   *  We encode the public key using the DER-encoded PKIX format
   *  We encode the private key as a PKCS1 key using ASN.1 DER.
   *
   *  according to openssl manual:
   *  https://www.openssl.org/docs/man1.1.1/man3/i2d_RSA_PUBKEY.html
   *  d2i_RSA_PUBKEY() and i2d_RSA_PUBKEY() decode and encode a PKIX
   *
   *  https://www.openssl.org/docs/man1.1.1/man3/i2d_RSAPrivateKey.html
   *  d2i_RSAPrivateKey(), i2d_RSAPrivateKey() decode and encode a PKCS#1
   */
  outcome::result<KeyPair> RsaProviderImpl::generate(
      RSAKeyType rsa_bitness) const {
    int bits{0};
    switch (rsa_bitness) {
      case RSAKeyType::RSA1024:
        bits = 1024;
        break;
      case RSAKeyType::RSA4096:
        bits = 4096;
        break;
      case RSAKeyType::RSA2048:
      default:
        bits = 2048;
    }

    int ret{0};
    RSA *rsa{nullptr};
    BIGNUM *bne{nullptr};
    // clean up automatically
    FinalAction cleanup([&] {
      if (nullptr != rsa) {
        RSA_free(rsa);
      }
      if (nullptr != bne) {
        BN_free(bne);
      }
    });

    constexpr uint64_t exp = RSA_F4;

    // 1. generate rsa state
    bne = BN_new();
    ret = BN_set_word(bne, exp);
    if (ret != 1) {
      return KeyGeneratorError::KEY_GENERATION_FAILED;
    }

    // 2. generate keys
    rsa = RSA_new();
    ret = RSA_generate_key_ex(rsa, bits, bne, nullptr);
    if (ret != 1) {
      return KeyGeneratorError::KEY_GENERATION_FAILED;
    }

    auto private_bytes_res = encodeKeyDer(rsa, i2d_RSAPrivateKey);
    if (!private_bytes_res) {
      return private_bytes_res.error();
    }
    auto private_bytes = std::move(private_bytes_res).value();

    auto public_bytes_res = encodeKeyDer(rsa, i2d_RSA_PUBKEY);
    if (!public_bytes_res) {
      return public_bytes_res.error();
    }
    auto public_bytes = std::move(public_bytes_res).value();

    return KeyPair{.private_key = private_bytes, .public_key = public_bytes};
  }

  outcome::result<PublicKey> RsaProviderImpl::derive(
      const PrivateKey &private_key) const {
    const unsigned char *data_pointer = private_key.data();
    RSA *rsa = d2i_RSAPrivateKey(
        nullptr, &data_pointer, static_cast<long>(private_key.size()));
    if (nullptr == rsa) {
      return KeyGeneratorError::KEY_DERIVATION_FAILED;
    }
    FinalAction cleanup_rsa([rsa]() { RSA_free(rsa); });

    auto public_bytes_res = encodeKeyDer(rsa, i2d_RSA_PUBKEY);
    if (!public_bytes_res) {
      return public_bytes_res.error();
    }
    auto public_bytes = std::move(public_bytes_res).value();

    return std::move(public_bytes);
  }

  outcome::result<std::shared_ptr<RSA>> rsaFromPrivateKey(
      const PrivateKey &private_key) {
    const unsigned char *data_pointer = private_key.data();
    std::shared_ptr<RSA> rsa{
        d2i_RSAPrivateKey(
            nullptr, &data_pointer, static_cast<long>(private_key.size())),
        RSA_free};
    if (nullptr == rsa) {
      return KeyValidatorError::INVALID_PRIVATE_KEY;
    }
    return rsa;
  }

  outcome::result<Signature> RsaProviderImpl::sign(
      BytesIn message, const PrivateKey &private_key) const {
    auto rsa_res = rsaFromPrivateKey(private_key);
    if (!rsa_res) {
      return rsa_res.error();
    }
    auto rsa = std::move(rsa_res).value();
    auto digest_res = sha256(message);
    if (!digest_res) {
      return digest_res.error();
    }
    auto digest = std::move(digest_res).value();
    Signature signature(RSA_size(rsa.get()));
    unsigned int signature_size = 0;
    if (1
        != RSA_sign(NID_sha256,
                    digest.data(),
                    digest.size(),
                    signature.data(),
                    &signature_size,
                    rsa.get())) {
      return CryptoProviderError::SIGNATURE_GENERATION_FAILED;
    }

    return signature;
  }

  outcome::result<bool> RsaProviderImpl::verify(
      BytesIn message,
      const Signature &signature,
      const PublicKey &public_key) const {
    auto x509_key_res = RsaProviderImpl::getPublicKeyFromBytes(public_key);
    if (!x509_key_res) {
      return x509_key_res.error();
    }
    auto x509_key = std::move(x509_key_res).value();

    EVP_PKEY *key = X509_PUBKEY_get(x509_key.get());
    if (!key) {
      return CryptoProviderError::SIGNATURE_VERIFICATION_FAILED;
    }
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key_ptr{key,
                                                                EVP_PKEY_free};
    std::unique_ptr<RSA, decltype(&RSA_free)> rsa{
        EVP_PKEY_get1_RSA(key_ptr.get()), RSA_free};
    if (!rsa) {
      return CryptoProviderError::SIGNATURE_VERIFICATION_FAILED;
    }

    auto digest_res = sha256(message);
    if (!digest_res) {
      return digest_res.error();
    }
    auto digest = std::move(digest_res).value();
    int result = RSA_verify(NID_sha256,
                            digest.data(),
                            digest.size(),
                            signature.data(),
                            signature.size(),
                            rsa.get());
    return 1 == result;
  }

  outcome::result<std::shared_ptr<X509_PUBKEY>>
  RsaProviderImpl::getPublicKeyFromBytes(const PublicKey &input_key) {
    const uint8_t *bytes = input_key.data();
    X509_PUBKEY *key_ptr = nullptr;
    if (d2i_X509_PUBKEY(&key_ptr, &bytes, static_cast<long>(input_key.size()))
        == nullptr) {
      return KeyValidatorError::INVALID_PUBLIC_KEY;
    }
    return std::shared_ptr<X509_PUBKEY>{key_ptr, X509_PUBKEY_free};
  }

};  // namespace libp2p::crypto::rsa
