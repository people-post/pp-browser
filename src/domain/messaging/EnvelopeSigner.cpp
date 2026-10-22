#include "domain/messaging/EnvelopeSigner.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"

#include "common/Serialize.hpp"

#include <sodium.h>

#include <array>
#include <cstring>
#include <sstream>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr std::array<char, 34> kSignDomain = {
    'p', 'p', '-', 'b', 'r', 'o', 'w', 's', 'e', 'r', ':', 'r', 'e', 'l', 'a', 'y', '-',
    'e', 'n', 'v', 'e', 'l', 'o', 'p', 'e', '-', 's', 'i', 'g', 'n', '-', 'v', '2', '\0'};

constexpr uint8_t kSignVersion = 2;
constexpr uint8_t kBodyKindE2e = 0x02;

void AppendBigEndianInt64(std::ostringstream& oss, const int64_t value) {
  OutputArchive ar(oss);
  ar & value;
}

void AppendBigEndianUInt64(std::ostringstream& oss, const uint64_t value) {
  OutputArchive ar(oss);
  ar & value;
}

void AppendBigEndianUInt32(std::ostringstream& oss, const uint32_t value) {
  OutputArchive ar(oss);
  ar & value;
}

} // namespace

uint8_t EnvelopeSigner::RouteKindByte(const RelayRoute& route) {
  if (route.kind == kRelayRouteGroup) {
    return 1;
  }
  if (route.kind == kRelayRouteControl) {
    return 2;
  }
  return 0;
}

uint8_t EnvelopeSigner::ChannelByte(const RelayRoute& route) {
  if (route.kind == kRelayRouteControl) {
    return 1;  // always e2e_public
  }
  if (route.kind != kRelayRouteDirect) {
    return 0xFF;
  }
  switch (route.channel) {
  case ThreadChannel::E2e:
    return 0;
  case ThreadChannel::E2ePublic:
    return 1;
  case ThreadChannel::None:
    return 0xFF;
  }
  return 0xFF;
}

Roe<void> EnvelopeSigner::CheckBodyShape(const RelayEnvelope& envelope) {
  const bool has_members = envelope.body.e2e.member_payloads.has_value();
  const bool has_payload = !envelope.body.e2e.payload_b64.empty();
  if (envelope.route.kind == kRelayRouteGroup) {
    if (!envelope.route.group_id || envelope.route.group_id->empty()) {
      return Error("Group envelope without group_id");
    }
    if (!has_members || envelope.body.e2e.member_payloads->empty() || has_payload) {
      return Error("Group envelope must carry member_payloads only");
    }
    return {};
  }
  if (envelope.route.kind == kRelayRouteDirect || envelope.route.kind == kRelayRouteControl) {
    if (has_members || !has_payload) {
      return Error("Direct / control envelope must carry payload_b64 only");
    }
    if (envelope.route.group_id) {
      return Error("Direct / control envelope with a group_id");
    }
    return {};
  }
  return Error("Unknown route kind");
}

Roe<std::vector<uint8_t>> EnvelopeSigner::BodyHash(const RelayEnvelope& envelope) {
  EnsureSodiumInit();
  if (auto shape = CheckBodyShape(envelope); !shape) {
    return shape.error();
  }
  // Exactly the bytes the receiver decrypts: the one blob of a direct / control envelope, or each
  // (member, blob) pair of a group envelope, length-prefixed so pairs cannot be re-split.
  std::ostringstream oss;
  oss.put(static_cast<char>(kBodyKindE2e));
  {
    OutputArchive ar(oss);
    if (envelope.route.kind == kRelayRouteGroup) {
      for (const auto& [member_id, payload_b64] : *envelope.body.e2e.member_payloads) {
        auto decoded = Base64Decode(payload_b64);
        if (!decoded) {
          return decoded.error();
        }
        WireLenUtf8 member{member_id};
        ar & member;
        WireLenBytes blob{*decoded};
        ar & blob;
      }
    } else {
      auto decoded = Base64Decode(envelope.body.e2e.payload_b64);
      if (!decoded) {
        return decoded.error();
      }
      WireLenBytes blob{*decoded};
      ar & blob;
    }
  }
  const std::string input = oss.str();
  std::vector<uint8_t> digest(32);
  if (crypto_generichash(digest.data(), digest.size(), reinterpret_cast<const unsigned char*>(input.data()),
                         input.size(), nullptr, 0) != 0) {
    return Error("BLAKE2b body hash failed");
  }
  return digest;
}

Roe<std::vector<uint8_t>> EnvelopeSigner::BuildSignBytes(const RelayEnvelope& envelope) {
  if (envelope.envelope_version != kRelayEnvelopeVersion) {
    return Error("Unsupported envelope version");
  }
  auto body_hash = BodyHash(envelope);
  if (!body_hash) {
    return body_hash.error();
  }

  std::ostringstream oss;
  oss.write(kSignDomain.data(), static_cast<std::streamsize>(kSignDomain.size()));
  const uint8_t sign_version = kSignVersion;
  const uint8_t envelope_version = static_cast<uint8_t>(envelope.envelope_version);
  const uint8_t route_kind = RouteKindByte(envelope.route);
  const uint8_t channel = ChannelByte(envelope.route);
  oss.write(reinterpret_cast<const char*>(&sign_version), 1);
  oss.write(reinterpret_cast<const char*>(&envelope_version), 1);
  oss.write(reinterpret_cast<const char*>(&route_kind), 1);
  oss.write(reinterpret_cast<const char*>(&channel), 1);
  AppendBigEndianInt64(oss, envelope.timestamp);
  AppendBigEndianUInt64(oss, envelope.sender_seq);
  AppendBigEndianUInt32(oss, envelope.session_epoch);
  oss.write(reinterpret_cast<const char*>(body_hash->data()), static_cast<std::streamsize>(body_hash->size()));

  {
    OutputArchive ar(oss);
    WireLenUtf8 message_id{envelope.message_id};
    ar & message_id;
    WireLenUtf8 sender_contact_id{envelope.sender_contact_id};
    ar & sender_contact_id;
    // v2: who the sender says it is on the relay, which group, and the AutoKey key_init — each
    // decides something on receipt (route, thread, session), so each is the signer's word.
    WireLenUtf8 sender_relay_id{envelope.sender_relay_id};
    ar & sender_relay_id;
    WireLenUtf8 group_id{envelope.route.group_id.value_or(std::string{})};
    ar & group_id;
    WireLenUtf8 key_init{envelope.body.e2e.key_init_b64.value_or(std::string{})};
    ar & key_init;
  }

  const std::string packed = oss.str();
  return std::vector<uint8_t>(packed.begin(), packed.end());
}

Roe<bool> EnvelopeSigner::Verify(const RelayEnvelope& envelope, const std::string& public_key_b64) {
  if (envelope.signature.empty()) {
    return Error("Missing envelope signature");
  }
  auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
  if (!sign_bytes) {
    return sign_bytes.error();
  }
  auto public_key = Base64Decode(public_key_b64);
  if (!public_key) {
    return public_key.error();
  }
  if (public_key.value().size() != kMlDsa65PublicKeyBytes) {
    return Error("Invalid ML-DSA-65 public key size");
  }
  auto signature = Base64Decode(envelope.signature);
  if (!signature) {
    return signature.error();
  }
  return MlDsa::Verify(public_key.value(), sign_bytes.value(), signature.value());
}

} // namespace pbr
