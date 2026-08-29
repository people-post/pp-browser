/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/crypto/crypto_provider/crypto_provider_impl.hpp>

#include <exception>

#include <openssl/ecdsa.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/mem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <libp2p/common/final_action.hpp>
#include <libp2p/crypto/ecdsa_provider.hpp>
#include <libp2p/crypto/ed25519_provider.hpp>
#include <libp2p/crypto/error.hpp>
#include <libp2p/crypto/hmac_provider.hpp>
#include <libp2p/crypto/mldsa_provider.hpp>
#include <libp2p/crypto/random_generator.hpp>
#include <libp2p/crypto/rsa_provider.hpp>
#include <span>

using libp2p::common::FinalAction;

OUTCOME_CPP_DEFINE_CATEGORY(libp2p::crypto, CryptoProviderImpl::Error, e) {
  using E = libp2p::crypto::CryptoProviderImpl::Error;
  switch (e) {
    case E::UNKNOWN_CIPHER_TYPE:
      return "Unsupported cipher algorithm type was specified";
    case E::UNKNOWN_HASH_TYPE:
      return "Unsupported hash algorithm type was specified";
  }
  return "Unknown error";
}

namespace libp2p::crypto {
  CryptoProviderImpl::CryptoProviderImpl(
      std::shared_ptr<random::CSPRNG> random_provider,
      std::shared_ptr<ed25519::Ed25519Provider> ed25519_provider,
      std::shared_ptr<rsa::RsaProvider> rsa_provider,
      std::shared_ptr<ecdsa::EcdsaProvider> ecdsa_provider,
      std::shared_ptr<hmac::HmacProvider> hmac_provider,
      std::shared_ptr<mldsa::MlDsaProvider> mldsa_provider)
      : random_provider_{std::move(random_provider)},
        ed25519_provider_{std::move(ed25519_provider)},
        rsa_provider_{std::move(rsa_provider)},
        ecdsa_provider_{std::move(ecdsa_provider)},
        hmac_provider_{std::move(hmac_provider)},
        mldsa_provider_{std::move(mldsa_provider)} {
    initialize();
  }

  void CryptoProviderImpl::initialize() {
    constexpr size_t kSeedBytesCount = 128 * 4;  // ripple uses such number
    auto bytes = random_provider_->randomBytes(kSeedBytesCount);
    // seeding random crypto_provider is required prior to calling
    // RSA_generate_key NOLINTNEXTLINE
    RAND_seed(static_cast<const void *>(bytes.data()), bytes.size());
  }

  /* ###################################################################
   *
   * Key Generation
   *
   * ################################################################### */

  outcome::result<KeyPair> CryptoProviderImpl::generateKeys(
      Key::Type key_type, common::RSAKeyType rsa_bitness) const {
    switch (key_type) {
      case Key::Type::RSA:
        return generateRsa(rsa_bitness);
      case Key::Type::Ed25519:
        return generateEd25519();
      case Key::Type::Secp256k1:
        return KeyGeneratorError::UNSUPPORTED_KEY_TYPE;
      case Key::Type::ECDSA:
        return generateEcdsa();
      case Key::Type::MlDsa65:
        return generateMlDsa65();
      default:
        return KeyGeneratorError::UNSUPPORTED_KEY_TYPE;
    }
  }

  outcome::result<KeyPair> CryptoProviderImpl::generateRsa(
      common::RSAKeyType rsa_bitness) const {
    auto rsa_res = rsa_provider_->generate(rsa_bitness);
    if (!rsa_res) {
      return rsa_res.as_failure();
    }
    auto rsa = std::move(rsa_res).value();

    auto &&pub = rsa.public_key;
    auto &&priv = rsa.private_key;
    return KeyPair{.publicKey = {{.type = Key::Type::RSA,
                                  .data = {pub.begin(), pub.end()}}},
                   .privateKey = {{.type = Key::Type::RSA,
                                   .data = {priv.begin(), priv.end()}}}};
  }

  outcome::result<KeyPair> CryptoProviderImpl::generateEd25519() const {
    auto ed_res = ed25519_provider_->generate();
    if (!ed_res) {
      return ed_res.as_failure();
    }
    auto ed = std::move(ed_res).value();

    auto &&pub = ed.public_key;
    auto &&priv = ed.private_key;
    return KeyPair{.publicKey = {{.type = Key::Type::Ed25519,
                                  .data = {pub.begin(), pub.end()}}},
                   .privateKey = {{.type = Key::Type::Ed25519,
                                   .data = {priv.begin(), priv.end()}}}};
  }

  outcome::result<KeyPair> CryptoProviderImpl::generateEcdsa() const {
    auto ecdsa_res = ecdsa_provider_->generate();
    if (!ecdsa_res) {
      return ecdsa_res.as_failure();
    }
    auto ecdsa = std::move(ecdsa_res).value();

    auto &&pub = ecdsa.public_key;
    auto &&priv = ecdsa.private_key;
    return KeyPair{.publicKey = {{.type = Key::Type::ECDSA,
                                  .data = {pub.begin(), pub.end()}}},
                   .privateKey = {{.type = Key::Type::ECDSA,
                                   .data = {priv.begin(), priv.end()}}}};
  }

  outcome::result<KeyPair> CryptoProviderImpl::generateMlDsa65() const {
    if (!mldsa_provider_) {
      return KeyGeneratorError::UNSUPPORTED_KEY_TYPE;
    }
    auto mldsa_res = mldsa_provider_->generate();
    if (!mldsa_res) {
      return mldsa_res.as_failure();
    }
    auto mldsa = std::move(mldsa_res).value();
    return KeyPair{.publicKey = {{.type = Key::Type::MlDsa65,
                                  .data = std::move(mldsa.public_key)}},
                   .privateKey = {{.type = Key::Type::MlDsa65,
                                   .data = std::move(mldsa.private_key)}}};
  }

  /* ###################################################################
   *
   * Public Key Derivation
   *
   * ################################################################### */

  outcome::result<PublicKey> CryptoProviderImpl::derivePublicKey(
      const PrivateKey &private_key) const {
    switch (private_key.type) {
      case Key::Type::RSA:
        return deriveRsa(private_key);
      case Key::Type::Ed25519:
        return deriveEd25519(private_key);
      case Key::Type::Secp256k1:
        return KeyGeneratorError::UNSUPPORTED_KEY_TYPE;
      case Key::Type::ECDSA:
        return deriveEcdsa(private_key);
      case Key::Type::MlDsa65:
        // FIPS 204 secret key does not embed a recoverable public key blob.
        return KeyGeneratorError::KEY_DERIVATION_FAILED;
      case Key::Type::UNSPECIFIED:
        return KeyGeneratorError::WRONG_KEY_TYPE;
    }
    return KeyGeneratorError::UNSUPPORTED_KEY_TYPE;
  }

  outcome::result<PublicKey> CryptoProviderImpl::deriveRsa(
      const PrivateKey &key) const {
    rsa::PrivateKey private_key;
    private_key.insert(private_key.end(), key.data.begin(), key.data.end());
    auto rsa_pub_res = rsa_provider_->derive(private_key);
    if (!rsa_pub_res) {
      return rsa_pub_res.as_failure();
    }
    auto rsa_pub = std::move(rsa_pub_res).value();
    return PublicKey{{key.type, {rsa_pub.begin(), rsa_pub.end()}}};
  }

  outcome::result<PublicKey> CryptoProviderImpl::deriveEd25519(
      const PrivateKey &key) const {
    ed25519::PrivateKey private_key;
    std::copy_n(key.data.begin(), private_key.size(), private_key.begin());
    auto ed_pub_res = ed25519_provider_->derive(private_key);
    if (!ed_pub_res) {
      return ed_pub_res.as_failure();
    }
    auto ed_pub = std::move(ed_pub_res).value();

    return PublicKey{{key.type, {ed_pub.begin(), ed_pub.end()}}};
  }

  outcome::result<PublicKey> CryptoProviderImpl::deriveEcdsa(
      const PrivateKey &key) const {
    ecdsa::PrivateKey private_key;
    std::copy_n(key.data.begin(), private_key.size(), private_key.begin());
    auto ecdsa_pub_res = ecdsa_provider_->derive(private_key);
    if (!ecdsa_pub_res) {
      return ecdsa_pub_res.as_failure();
    }
    auto ecdsa_pub = std::move(ecdsa_pub_res).value();

    return PublicKey{{key.type, {ecdsa_pub.begin(), ecdsa_pub.end()}}};
  }

  /* ###################################################################
   *
   * Messages Signing
   *
   * ################################################################### */

  outcome::result<Buffer> CryptoProviderImpl::sign(
      BytesIn message, const PrivateKey &private_key) const {
    switch (private_key.type) {
      case Key::Type::RSA:
        return signRsa(message, private_key);
      case Key::Type::Ed25519:
        return signEd25519(message, private_key);
      case Key::Type::Secp256k1:
        return KeyGeneratorError::UNSUPPORTED_KEY_TYPE;
      case Key::Type::ECDSA:
        return signEcdsa(message, private_key);
      case Key::Type::MlDsa65:
        return signMlDsa65(message, private_key);
      case Key::Type::UNSPECIFIED:
        return KeyGeneratorError::WRONG_KEY_TYPE;
      default:
        return CryptoProviderError::SIGNATURE_GENERATION_FAILED;
    }
  }

  outcome::result<Buffer> CryptoProviderImpl::signRsa(
      BytesIn message, const PrivateKey &private_key) const {
    rsa::PrivateKey priv_key;
    priv_key.insert(
        priv_key.end(), private_key.data.begin(), private_key.data.end());

    auto signature_res = rsa_provider_->sign(message, priv_key);
    if (!signature_res) {
      return signature_res.error();
    }
    auto signature = std::move(signature_res).value();
    return {signature.begin(), signature.end()};
  }

  outcome::result<Buffer> CryptoProviderImpl::signEd25519(
      BytesIn message, const PrivateKey &private_key) const {
    ed25519::PrivateKey priv_key;
    std::copy_n(private_key.data.begin(), priv_key.size(), priv_key.begin());
    auto signature_res = ed25519_provider_->sign(message, priv_key);
    if (!signature_res) {
      return signature_res.error();
    }
    auto signature = std::move(signature_res).value();
    return {signature.begin(), signature.end()};
  }

  outcome::result<Buffer> CryptoProviderImpl::signEcdsa(
      BytesIn message, const PrivateKey &private_key) const {
    ecdsa::PrivateKey priv_key;
    std::copy_n(private_key.data.begin(), priv_key.size(), priv_key.begin());
    auto signature_res = ecdsa_provider_->sign(message, priv_key);
    if (!signature_res) {
      return signature_res.error();
    }
    auto signature = std::move(signature_res).value();
    return {signature.begin(), signature.end()};
  }

  /* ###################################################################
   *
   * Signatures Verifying
   *
   * ################################################################### */

  outcome::result<bool> CryptoProviderImpl::verify(
      BytesIn message, BytesIn signature, const PublicKey &public_key) const {
    switch (public_key.type) {
      case Key::Type::RSA:
        return verifyRsa(message, signature, public_key);
      case Key::Type::Ed25519:
        return verifyEd25519(message, signature, public_key);
      case Key::Type::Secp256k1:
        return KeyGeneratorError::UNSUPPORTED_KEY_TYPE;
      case Key::Type::ECDSA:
        return verifyEcdsa(message, signature, public_key);
      case Key::Type::MlDsa65:
        return verifyMlDsa65(message, signature, public_key);
      case Key::Type::UNSPECIFIED:
        return KeyGeneratorError::WRONG_KEY_TYPE;
      default:
        return CryptoProviderError::SIGNATURE_VERIFICATION_FAILED;
    }
  }

  outcome::result<bool> CryptoProviderImpl::verifyRsa(
      BytesIn message, BytesIn signature, const PublicKey &public_key) const {
    rsa::PublicKey rsa_pub;
    rsa_pub.insert(
        rsa_pub.end(), public_key.data.begin(), public_key.data.end());

    rsa::Signature rsa_sig;
    rsa_sig.insert(rsa_sig.end(), signature.begin(), signature.end());

    return rsa_provider_->verify(message, rsa_sig, rsa_pub);
  }

  outcome::result<bool> CryptoProviderImpl::verifyEd25519(
      BytesIn message, BytesIn signature, const PublicKey &public_key) const {
    ed25519::PublicKey ed_pub;
    std::copy_n(public_key.data.begin(), ed_pub.size(), ed_pub.begin());

    ed25519::Signature ed_sig;
    std::copy_n(signature.begin(), ed_sig.size(), ed_sig.begin());

    return ed25519_provider_->verify(message, ed_sig, ed_pub);
  }

  outcome::result<bool> CryptoProviderImpl::verifyEcdsa(
      BytesIn message, BytesIn signature, const PublicKey &public_key) const {
    ecdsa::PublicKey ecdsa_pub;
    std::copy_n(public_key.data.begin(), ecdsa_pub.size(), ecdsa_pub.begin());

    ecdsa::Signature ecdsa_sig;
    ecdsa_sig.insert(ecdsa_sig.end(), signature.begin(), signature.end());

    return ecdsa_provider_->verify(message, ecdsa_sig, ecdsa_pub);
  }

  outcome::result<Buffer> CryptoProviderImpl::signMlDsa65(
      BytesIn message, const PrivateKey &private_key) const {
    if (!mldsa_provider_) {
      return CryptoProviderError::SIGNATURE_GENERATION_FAILED;
    }
    auto signature_res =
        mldsa_provider_->sign(message, private_key.data);
    if (!signature_res) {
      return signature_res.error();
    }
    return std::move(signature_res).value();
  }

  outcome::result<bool> CryptoProviderImpl::verifyMlDsa65(
      BytesIn message, BytesIn signature, const PublicKey &public_key) const {
    if (!mldsa_provider_) {
      return CryptoProviderError::SIGNATURE_VERIFICATION_FAILED;
    }
    mldsa::Signature mldsa_sig(signature.begin(), signature.end());
    return mldsa_provider_->verify(message, mldsa_sig, public_key.data);
  }

  /* ###################################################################
   *
   * Ephemeral Key Cryptography
   *
   * ################################################################### */

  outcome::result<EphemeralKeyPair>
  CryptoProviderImpl::generateEphemeralKeyPair(common::CurveType curve) const {
    const auto FAILED{KeyGeneratorError::KEY_GENERATION_FAILED};
    int nid = 0;
    size_t private_key_length_in_bytes = 0;
    switch (curve) {
      case common::CurveType::P256:
        nid = NID_X9_62_prime256v1;
        private_key_length_in_bytes = 32;
        break;
      case common::CurveType::P384:
        nid = NID_secp384r1;
        private_key_length_in_bytes = 48;
        break;
      case common::CurveType::P521:
        nid = NID_secp521r1;
        private_key_length_in_bytes = 66;  // 66 * 8 = 528 > 521
        break;
      default:
        return KeyGeneratorError::UNSUPPORTED_CURVE_TYPE;
    }

    // allocate key
    EC_KEY *key = EC_KEY_new();
    if (nullptr == key) {
      return FAILED;
    }
    FinalAction free_private_key([key] { EC_KEY_free(key); });

    // create curve
    EC_GROUP *curve_group = EC_GROUP_new_by_curve_name(nid);
    if (nullptr == curve_group) {
      return FAILED;
    }
    FinalAction free_curve_group([curve_group] { EC_GROUP_free(curve_group); });

    // assign curve to the key
    if (1 != EC_KEY_set_group(key, curve_group)) {
      return FAILED;
    }
    // generate the key
    if (1 != EC_KEY_generate_key(key)) {
      return FAILED;
    }

    // check that private key field is set
    const BIGNUM *private_key_bignum = EC_KEY_get0_private_key(key);
    if (nullptr == private_key_bignum) {
      return FAILED;
    }

    // serialize private key bytes and move them later to shared secret
    // generator
    std::vector<uint8_t> private_bytes(private_key_length_in_bytes, 0);
    // serialize to buffer
    if (BN_bn2binpad(private_key_bignum,
                     private_bytes.data(),
                     private_key_length_in_bytes)
        < 0) {
      return FAILED;
    }

    // check that public key field is also set
    const EC_POINT *public_key_point = EC_KEY_get0_public_key(key);
    if (nullptr == public_key_point) {
      /*
       * Try to compute valid public key if it was not created by keygen for
       * some reason.
       *
       * This is written for safety purposes since EC_KEY_generate_key docstring
       * says the following: "Creates a new ec private (and optional a new
       * public) key."
       */
      EC_POINT *computed_public_key_point{nullptr};
      if (1
          != EC_POINT_mul(curve_group,
                          computed_public_key_point,
                          private_key_bignum,
                          nullptr,
                          nullptr,
                          nullptr)) {
        return FAILED;
      }
      if (1 != EC_KEY_set_public_key(key, computed_public_key_point)) {
        return FAILED;
      }
    }

    // marshall pubkey point to bytes buffer (uncompressed form)
    const EC_POINT *public_key{EC_KEY_get0_public_key(key)};
    uint8_t *pubkey_bytes_buffer{nullptr};
    FinalAction free_pubkey_bytes_buffer([pptr = &pubkey_bytes_buffer]() {
      if (*pptr != nullptr) {
        OPENSSL_free(*pptr);
      }
    });

    size_t pubkey_bytes_length{0};
    if (0
        == (pubkey_bytes_length =
                EC_POINT_point2buf(curve_group,
                                   public_key,
                                   POINT_CONVERSION_UNCOMPRESSED,
                                   &pubkey_bytes_buffer,
                                   nullptr))) {
      return FAILED;
    }

    auto pubkey_span = std::span(pubkey_bytes_buffer, pubkey_bytes_length);
    Buffer pubkey_buffer(pubkey_span.begin(), pubkey_span.end());

    return EphemeralKeyPair{
        .ephemeral_public_key = std::move(pubkey_buffer),
        .shared_secret_generator =
            prepareSharedSecretGenerator(nid, std::move(private_bytes))};
  }

  std::function<outcome::result<Buffer>(Buffer)>
  CryptoProviderImpl::prepareSharedSecretGenerator(int curve_nid,
                                                   Buffer own_private_key) {
    return [curve_nid, private_bytes{std::move(own_private_key)}](
               Buffer their_epubkey) -> outcome::result<Buffer> {
      const auto FAILED{KeyGeneratorError::KEY_DERIVATION_FAILED};
      // init curve
      EC_GROUP *curve_group = EC_GROUP_new_by_curve_name(curve_nid);
      if (nullptr == curve_group) {
        return FAILED;
      }
      FinalAction free_curve_group(
          [curve_group] { EC_GROUP_free(curve_group); });

      // create empty key
      EC_KEY *their_key = EC_KEY_new();
      if (nullptr == their_key) {
        return FAILED;
      }
      FinalAction free_their_key([their_key] { EC_KEY_free(their_key); });

      // assign curve type to the key
      if (1 != EC_KEY_set_group(their_key, curve_group)) {
        return FAILED;
      }

      // restore their epubkey from bytes
      const unsigned char *their_epubkey_buffer{their_epubkey.data()};
      if (nullptr
          == o2i_ECPublicKey(
              &their_key, &their_epubkey_buffer, their_epubkey.size())) {
        return FAILED;
      }

      // get pubkey in format of point for future computations
      const EC_POINT *their_pubkey_point{EC_KEY_get0_public_key(their_key)};
      if (nullptr == their_pubkey_point) {
        return FAILED;
      }

      if (1 != EC_POINT_is_on_curve(curve_group, their_pubkey_point, nullptr)) {
        return FAILED;
      }

      // restore private key bignum from bytes
      // create new bignum for storing private key
      BIGNUM *private_bignum = BN_new();
      if (nullptr == private_bignum) {
        return FAILED;
      }
      // auto cleanup for bignum
      FinalAction free_private_bignum(
          [private_bignum]() { BN_free(private_bignum); });
      // convert private key bytes to BIGNUM
      const unsigned char *private_key_buffer = private_bytes.data();
      if (nullptr
          == BN_bin2bn(
              private_key_buffer, private_bytes.size(), private_bignum)) {
        return FAILED;
      }

      EC_KEY *our_private_key = EC_KEY_new_by_curve_name(curve_nid);
      if (nullptr == our_private_key) {
        return FAILED;
      }
      FinalAction free_our_private_key(
          [our_private_key] { EC_KEY_free(our_private_key); });

      if (1 != EC_KEY_set_private_key(our_private_key, private_bignum)) {
        return FAILED;
      }

      // calculate the size of the buffer for the shared secret
      auto field_size = static_cast<int>(EC_GROUP_get_degree(curve_group));
      int secret_len{(field_size + 7) / 8};
      uint8_t *secret_buffer{(uint8_t *)OPENSSL_malloc(secret_len)};  // NOLINT
      if (nullptr == secret_buffer) {
        return FAILED;
      }

      secret_len = ECDH_compute_key(secret_buffer,
                                    secret_len,
                                    their_pubkey_point,
                                    our_private_key,
                                    nullptr);
      if (secret_len <= 0) {
        OPENSSL_free(secret_buffer);
        // ^ comment to the condition above:
        // doc says it might be negative when error,
        // openssl sources show it will be zero when error
        return FAILED;
      }

      auto secret_span = std::span(secret_buffer, secret_len);
      Buffer secret(secret_span.begin(), secret_span.end());
      OPENSSL_free(secret_buffer);
      return secret;
    };
  }

  outcome::result<std::pair<StretchedKey, StretchedKey>>
  CryptoProviderImpl::stretchKey(common::CipherType cipher_type,
                                 common::HashType hash_type,
                                 const Buffer &secret) const {
    size_t cipher_key_size = 0;
    size_t iv_size = 0;
    switch (cipher_type) {
      case common::CipherType::AES128:
        cipher_key_size = 16;
        iv_size = 16;
        break;
      case common::CipherType::AES256:
        cipher_key_size = 32;
        iv_size = 16;
        break;
      default:
        return Error::UNKNOWN_CIPHER_TYPE;
    }

    constexpr size_t hmac_key_size{20};
    Buffer seed{
        'k', 'e', 'y', ' ', 'e', 'x', 'p', 'a', 'n', 's', 'i', 'o', 'n'};

    size_t output_size{2 * (iv_size + cipher_key_size + hmac_key_size)};
    Buffer result;
    result.reserve(output_size);

    auto a_res = hmac_provider_->calculateDigest(hash_type, secret, seed);
    if (!a_res) {
      return a_res.error();
    }
    auto a = std::move(a_res).value();
    while (result.size() < output_size) {
      Buffer input;
      input.reserve(a.size() + seed.size());
      input.insert(input.end(), a.begin(), a.end());
      input.insert(input.end(), seed.begin(), seed.end());

      auto b_res = hmac_provider_->calculateDigest(hash_type, secret, input);
      if (!b_res) {
        return b_res.error();
      }
      auto b = std::move(b_res).value();
      size_t todo = b.size();
      if (result.size() + todo > output_size) {
        todo = output_size - result.size();
      }
      std::copy_n(b.begin(), todo, std::back_inserter(result));
      auto c_res = hmac_provider_->calculateDigest(hash_type, secret, a);
      if (!c_res) {
        return c_res.error();
      }
      a = std::move(c_res).value();
    }

    auto iter = result.begin();
    Buffer k1_iv{iter, iter + iv_size};
    iter += iv_size;
    Buffer k1_cipher_key{iter, iter + cipher_key_size};
    iter += cipher_key_size;
    Buffer k1_mac_key{iter, iter + hmac_key_size};
    iter += hmac_key_size;

    Buffer k2_iv{iter, iter + iv_size};
    iter += iv_size;
    Buffer k2_cipher_key{iter, iter + cipher_key_size};
    iter += cipher_key_size;
    Buffer k2_mac_key{iter, iter + hmac_key_size};

    return std::make_pair(StretchedKey{.iv = std::move(k1_iv),
                                       .cipher_key = std::move(k1_cipher_key),
                                       .mac_key = std::move(k1_mac_key)},
                          StretchedKey{.iv = std::move(k2_iv),
                                       .cipher_key = std::move(k2_cipher_key),
                                       .mac_key = std::move(k2_mac_key)});
  }

}  // namespace libp2p::crypto
