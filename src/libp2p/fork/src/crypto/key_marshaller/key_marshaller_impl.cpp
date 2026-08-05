/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/crypto/key_marshaller/key_marshaller_impl.hpp>

#include <libp2p/crypto/common.hpp>
#include <libp2p/crypto/crypto_provider.hpp>
#include <libp2p/wire/keys_wire.hpp>

namespace libp2p::crypto::marshaller {
  namespace {
    outcome::result<wire::KeyTypeWire> marshalKeyType(Key::Type type) {
      switch (type) {
        case Key::Type::RSA:
          return wire::KeyTypeWire::kRsa;
        case Key::Type::Ed25519:
          return wire::KeyTypeWire::kEd25519;
        case Key::Type::Secp256k1:
          return wire::KeyTypeWire::kSecp256k1;
        case Key::Type::ECDSA:
          return wire::KeyTypeWire::kEcdsa;
        case Key::Type::UNSPECIFIED:
          return CryptoProviderError::INVALID_KEY_TYPE;
      }

      return CryptoProviderError::UNKNOWN_KEY_TYPE;
    }

    outcome::result<Key::Type> unmarshalKeyType(wire::KeyTypeWire type) {
      switch (type) {
        case wire::KeyTypeWire::kRsa:
          return Key::Type::RSA;
        case wire::KeyTypeWire::kEd25519:
          return Key::Type::Ed25519;
        case wire::KeyTypeWire::kSecp256k1:
          return Key::Type::Secp256k1;
        case wire::KeyTypeWire::kEcdsa:
          return Key::Type::ECDSA;
      }
      return CryptoProviderError::UNKNOWN_KEY_TYPE;
    }
  }  // namespace

  KeyMarshallerImpl::KeyMarshallerImpl(
      std::shared_ptr<validator::KeyValidator> key_validator)
      : key_validator_{std::move(key_validator)} {}

  outcome::result<ProtobufKey> KeyMarshallerImpl::marshal(
      const PublicKey &key) const {
    wire::PublicKeyWire wire_key;
    auto type_res = marshalKeyType(key.type);
    if (not type_res) {
      return std::move(type_res).as_failure();
    }
    wire_key.type = std::move(type_res).value();
    wire_key.data.assign(key.data.begin(), key.data.end());

    auto encoded = wire_key.encode();
    if (not encoded) {
      return CryptoProviderError::FAILED_UNMARSHAL_DATA;
    }
    return ProtobufKey{std::move(encoded.value())};
  }

  outcome::result<ProtobufKey> KeyMarshallerImpl::marshal(
      const PrivateKey &key) const {
    wire::PrivateKeyWire wire_key;
    auto type_res = marshalKeyType(key.type);
    if (not type_res) {
      return std::move(type_res).as_failure();
    }
    wire_key.type = std::move(type_res).value();
    wire_key.data.assign(key.data.begin(), key.data.end());

    auto encoded = wire_key.encode();
    if (not encoded) {
      return CryptoProviderError::FAILED_UNMARSHAL_DATA;
    }
    return ProtobufKey{std::move(encoded.value())};
  }

  outcome::result<PublicKey> KeyMarshallerImpl::unmarshalPublicKey(
      const ProtobufKey &proto_key) const {
    auto wire_key_res = wire::PublicKeyWire::decode(proto_key.key);
    if (!wire_key_res) {
      return CryptoProviderError::FAILED_UNMARSHAL_DATA;
    }
    auto &&wire_key = wire_key_res.value();

    auto type_res = unmarshalKeyType(wire_key.type);
    if (not type_res) {
      return std::move(type_res).as_failure();
    }
    auto type = std::move(type_res).value();
    auto key = PublicKey{{type, {wire_key.data.begin(), wire_key.data.end()}}};

    auto validate_res = key_validator_->validate(key);
    if (not validate_res) {
      return std::move(validate_res).as_failure();
    }

    return key;
  }

  outcome::result<PrivateKey> KeyMarshallerImpl::unmarshalPrivateKey(
      const ProtobufKey &proto_key) const {
    auto wire_key_res = wire::PrivateKeyWire::decode(proto_key.key);
    if (!wire_key_res) {
      return CryptoProviderError::FAILED_UNMARSHAL_DATA;
    }
    auto &&wire_key = wire_key_res.value();

    auto type_res = unmarshalKeyType(wire_key.type);
    if (not type_res) {
      return std::move(type_res).as_failure();
    }
    auto type = std::move(type_res).value();
    auto key = PrivateKey{{type, {wire_key.data.begin(), wire_key.data.end()}}};

    auto validate_res = key_validator_->validate(key);
    if (not validate_res) {
      return std::move(validate_res).as_failure();
    }

    return key;
  }

}  // namespace libp2p::crypto::marshaller
