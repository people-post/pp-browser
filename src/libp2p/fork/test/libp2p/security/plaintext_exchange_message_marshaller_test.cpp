/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gtest/gtest.h>
#include <libp2p/crypto/key.hpp>
#include <libp2p/peer/peer_id.hpp>
#include <libp2p/security/plaintext/exchange_message_marshaller_impl.hpp>
#include <libp2p/wire/keys_wire.hpp>
#include <qtils/test/outcome.hpp>
#include "mock/libp2p/crypto/key_marshaller_mock.hpp"

using libp2p::crypto::Key;
using libp2p::crypto::ProtobufKey;
using libp2p::crypto::PublicKey;
using libp2p::crypto::marshaller::KeyMarshallerMock;
using libp2p::peer::PeerId;
using libp2p::security::plaintext::ExchangeMessage;
using libp2p::security::plaintext::ExchangeMessageMarshaller;
using libp2p::security::plaintext::ExchangeMessageMarshallerImpl;
using libp2p::wire::KeyTypeWire;
using libp2p::wire::PublicKeyWire;
using testing::_;
using testing::Return;

class ExchangeMessageMarshallerTest : public testing::Test {
 public:
  void SetUp() {
    key_marshaller = std::make_shared<KeyMarshallerMock>();
    PublicKeyWire wire_pk;
    wire_pk.type = KeyTypeWire::kEd25519;
    wire_pk.data = pk.data;
    auto encoded = wire_pk.encode();
    ASSERT_TRUE(encoded);
    pubkey_bytes = std::move(encoded.value());
    marshaller =
        std::make_shared<ExchangeMessageMarshallerImpl>(key_marshaller);
  }

  std::shared_ptr<KeyMarshallerMock> key_marshaller;
  std::shared_ptr<ExchangeMessageMarshaller> marshaller;
  PublicKey pk{{Key::Type::Ed25519, std::vector<uint8_t>(255, 1)}};
  std::vector<uint8_t> pubkey_bytes;
};

TEST_F(ExchangeMessageMarshallerTest, ToWireAndBack) {
  EXPECT_CALL(*key_marshaller, marshal(pk))
      .WillOnce(Return(ProtobufKey{pubkey_bytes}));
  EXPECT_CALL(*key_marshaller, unmarshalPublicKey(_)).WillOnce(Return(pk));
  ASSERT_OUTCOME_SUCCESS(pid, PeerId::fromPublicKey(ProtobufKey{pk.data}));
  ExchangeMessage msg{.pubkey = pk, .peer_id = pid};
  ASSERT_OUTCOME_SUCCESS(bytes, marshaller->marshal(msg));
  ASSERT_OUTCOME_SUCCESS(dec_msg, marshaller->unmarshal(bytes));
  ASSERT_EQ(msg.peer_id, dec_msg.first.peer_id);
  ASSERT_EQ(msg.pubkey, dec_msg.first.pubkey);
}

TEST_F(ExchangeMessageMarshallerTest, MarshalError) {
  EXPECT_CALL(*key_marshaller, marshal(pk))
      .WillOnce(Return(ProtobufKey{std::vector<uint8_t>(32, 1)}));
  ASSERT_OUTCOME_SUCCESS(pid, PeerId::fromPublicKey(ProtobufKey{pk.data}));
  ExchangeMessage msg{.pubkey = pk, .peer_id = pid};
  EXPECT_OUTCOME_ERROR(marshaller->marshal(msg));
}

TEST_F(ExchangeMessageMarshallerTest, UnmarshalError) {
  EXPECT_CALL(*key_marshaller, marshal(pk))
      .WillOnce(Return(ProtobufKey{pubkey_bytes}));
  EXPECT_CALL(*key_marshaller, unmarshalPublicKey(_))
      .WillOnce(
          Return(libp2p::crypto::CryptoProviderError::FAILED_UNMARSHAL_DATA));
  ASSERT_OUTCOME_SUCCESS(pid, PeerId::fromPublicKey(ProtobufKey{pk.data}));
  ExchangeMessage msg{.pubkey = pk, .peer_id = pid};
  ASSERT_OUTCOME_SUCCESS(bytes, marshaller->marshal(msg));
  EXPECT_OUTCOME_ERROR(marshaller->unmarshal(bytes));
}
