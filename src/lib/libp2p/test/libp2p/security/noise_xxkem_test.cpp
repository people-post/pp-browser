/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gtest/gtest.h>

#include <libp2p/log/configurator.hpp>
#include <libp2p/log/logger.hpp>
#include <libp2p/security/noise/crypto/cipher_suite.hpp>
#include <libp2p/security/noise/crypto/message_patterns.hpp>
#include <libp2p/security/noise/crypto/noise_ccp1305.hpp>
#include <libp2p/security/noise/crypto/noise_dh.hpp>
#include <libp2p/security/noise/crypto/noise_sha256.hpp>
#include <libp2p/security/noise/crypto/state.hpp>

#include <mutex>

#include <soralog/impl/configurator_from_yaml.hpp>

using namespace libp2p::security::noise;

namespace {
  void EnsureLogging() {
    static std::once_flag once;
    std::call_once(once, [] {
      static const std::string kCfg = R"(
sinks:
  - name: console
    type: console
    color: false
groups:
  - name: libp2p
    sink: console
    level: error
)";
      auto logging_system = std::make_shared<soralog::LoggingSystem>(
          std::make_shared<soralog::ConfiguratorFromYAML>(
              std::make_shared<libp2p::log::Configurator>(), kCfg));
      if (!logging_system->configure().has_error) {
        libp2p::log::setLoggingSystem(logging_system);
      }
    });
  }

  std::shared_ptr<CipherSuite> MakeSuite() {
    EnsureLogging();
    return std::make_shared<CipherSuiteImpl>(
        std::make_shared<NoiseDiffieHellmanImpl>(),
        std::make_shared<NoiseSHA256HasherImpl>(),
        std::make_shared<NamedCCPImpl>());
  }
}  // namespace

TEST(NoiseXXkem, TwoPeerHandshakeProducesCipherStates) {
  auto suite_i = MakeSuite();
  auto suite_r = MakeSuite();
  auto static_i = suite_i->generate().value();
  auto static_r = suite_r->generate().value();

  HandshakeState initiator;
  HandshakeState responder;
  ASSERT_TRUE(initiator.init(
      HandshakeStateConfig(suite_i, handshakeXX, true, static_i)));
  ASSERT_TRUE(responder.init(
      HandshakeStateConfig(suite_r, handshakeXX, false, static_r)));

  // Msg 1: -> e
  auto m1 = initiator.writeMessage({}, {}).value();
  auto r1 = responder.readMessage({}, m1.data).value();
  EXPECT_TRUE(r1.data.empty());

  // Msg 2: <- e, ee, s, es (+ empty payload)
  auto m2 = responder.writeMessage({}, {}).value();
  auto r2 = initiator.readMessage({}, m2.data).value();
  EXPECT_TRUE(r2.data.empty());

  // Msg 3: -> s, se (+ empty payload) → split
  auto m3 = initiator.writeMessage({}, {}).value();
  EXPECT_TRUE(m3.cs1);
  EXPECT_TRUE(m3.cs2);
  auto r3 = responder.readMessage({}, m3.data).value();
  EXPECT_TRUE(r3.cs1);
  EXPECT_TRUE(r3.cs2);

  // Transport AEAD round-trip under derived keys
  static constexpr uint8_t kPlain[] = {'p', 'q', '-', 'o', 'k'};
  auto ct = m3.cs1->encrypt({}, kPlain, {}).value();
  auto pt = r3.cs1->decrypt({}, ct, {}).value();
  EXPECT_EQ(pt, (libp2p::Bytes{kPlain, kPlain + sizeof(kPlain)}));
}

TEST(NoiseXXkem, SuiteNameIsXXkemMlKem) {
  auto suite = MakeSuite();
  EXPECT_EQ(suite->dhName(), "MLKEM768");
  EXPECT_EQ(suite->name(), "MLKEM768_ChaChaPoly_SHA256");
  EXPECT_EQ(handshakeXX.name, "XXkem");
  EXPECT_EQ(suite->dhSize(), 1184);
  EXPECT_EQ(suite->ciphertextSize(), 1088);
}
