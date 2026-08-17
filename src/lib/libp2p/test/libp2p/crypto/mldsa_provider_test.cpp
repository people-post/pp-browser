/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gtest/gtest.h>

#include <libp2p/crypto/crypto_provider/crypto_provider_impl.hpp>
#include <libp2p/crypto/ecdsa_provider/ecdsa_provider_impl.hpp>
#include <libp2p/crypto/ed25519_provider/ed25519_provider_impl.hpp>
#include <libp2p/crypto/hmac_provider/hmac_provider_impl.hpp>
#include <libp2p/crypto/key_marshaller/key_marshaller_impl.hpp>
#include <libp2p/crypto/key_validator/key_validator_impl.hpp>
#include <libp2p/crypto/mldsa_provider/mldsa_provider_impl.hpp>
#include <libp2p/crypto/random_generator/boost_generator.hpp>
#include <libp2p/crypto/rsa_provider/rsa_provider_impl.hpp>
#include <libp2p/crypto/secp256k1_provider/secp256k1_provider_impl.hpp>
#include <libp2p/peer/peer_id.hpp>

using namespace libp2p::crypto;

namespace {
  std::shared_ptr<CryptoProvider> MakeProvider() {
    auto csprng = std::make_shared<random::BoostRandomGenerator>();
    auto ed25519 = std::make_shared<ed25519::Ed25519ProviderImpl>();
    auto rsa = std::make_shared<rsa::RsaProviderImpl>();
    auto ecdsa = std::make_shared<ecdsa::EcdsaProviderImpl>();
    auto secp = std::make_shared<secp256k1::Secp256k1ProviderImpl>(csprng);
    auto hmac = std::make_shared<hmac::HmacProviderImpl>();
    auto mldsa = std::make_shared<mldsa::MlDsaProviderImpl>();
    return std::make_shared<CryptoProviderImpl>(
        csprng, ed25519, rsa, ecdsa, secp, hmac, mldsa);
  }
}  // namespace

TEST(MlDsaProvider, GenerateSignVerify) {
  auto provider = MakeProvider();
  auto keys_res = provider->generateKeys(Key::Type::MlDsa65);
  ASSERT_TRUE(keys_res);
  const auto &keys = keys_res.value();
  EXPECT_EQ(keys.publicKey.type, Key::Type::MlDsa65);
  EXPECT_EQ(keys.publicKey.data.size(), mldsa::kPublicKeyBytes);
  EXPECT_EQ(keys.privateKey.data.size(), mldsa::kPrivateKeyBytes);

  static constexpr uint8_t kMsg[] = {'h', 'e', 'l', 'l', 'o'};
  auto sig = provider->sign(kMsg, keys.privateKey);
  ASSERT_TRUE(sig);
  EXPECT_EQ(sig.value().size(), mldsa::kSignatureBytes);
  auto ok = provider->verify(kMsg, sig.value(), keys.publicKey);
  ASSERT_TRUE(ok);
  EXPECT_TRUE(ok.value());
}

TEST(MlDsaProvider, MarshalPeerIdRoundTrip) {
  auto provider = MakeProvider();
  auto keys_res = provider->generateKeys(Key::Type::MlDsa65);
  ASSERT_TRUE(keys_res);
  const auto &keys = keys_res.value();
  auto validator = std::make_shared<validator::KeyValidatorImpl>(provider);
  auto marshaller = std::make_shared<marshaller::KeyMarshallerImpl>(validator);
  auto marshalled = marshaller->marshal(keys.publicKey);
  ASSERT_TRUE(marshalled);
  auto peer = libp2p::peer::PeerId::fromPublicKey(marshalled.value());
  ASSERT_TRUE(peer);
  EXPECT_FALSE(peer.value().toBase58().empty());

  auto unmarshalled = marshaller->unmarshalPublicKey(marshalled.value());
  ASSERT_TRUE(unmarshalled);
  EXPECT_EQ(unmarshalled.value().type, Key::Type::MlDsa65);
  EXPECT_EQ(unmarshalled.value().data, keys.publicKey.data);
}

TEST(MlDsaProvider, KeyPairValidates) {
  auto provider = MakeProvider();
  auto keys_res = provider->generateKeys(Key::Type::MlDsa65);
  ASSERT_TRUE(keys_res);
  auto validator = std::make_shared<validator::KeyValidatorImpl>(provider);
  auto ok = validator->validate(keys_res.value());
  ASSERT_TRUE(ok);
}
