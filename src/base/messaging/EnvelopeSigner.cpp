#include "base/messaging/EnvelopeSigner.h"

#include "base/crypto/CryptoUtil.h"
#include "base/people/Ed25519Signer.h"

#include "common/Serialize.hpp"

#include <sodium.h>

#include <array>
#include <cstring>
#include <sstream>

namespace pbr {

namespace {

constexpr std::array<char, 34> kSignDomain = {
    'p', 'p', '-', 'b', 'r', 'o', 'w', 's', 'e', 'r', ':', 'r', 'e', 'l', 'a', 'y', '-',
    'e', 'n', 'v', 'e', 'l', 'o', 'p', 'e', '-', 's', 'i', 'g', 'n', '-', 'v', '1', '\0'};

constexpr uint8_t kSignVersion = 1;
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
  if (route.kind == "group") {
    return 1;
  }
  return 0;
}

uint8_t EnvelopeSigner::ChannelByte(const RelayRoute& route) {
  if (route.kind != "direct") {
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

Roe<std::vector<uint8_t>> EnvelopeSigner::BodyHash(const RelayMessageBody& body) {
  if (body.e2e.payload_b64.empty()) {
    return Error("Missing E2E payload for body hash");
  }
  auto decoded = Base64Decode(body.e2e.payload_b64);
  if (!decoded) {
    return decoded.error();
  }
  EnsureSodiumInit();
  std::vector<uint8_t> input;
  input.reserve(1 + decoded->size());
  input.push_back(kBodyKindE2e);
  input.insert(input.end(), decoded->begin(), decoded->end());

  std::vector<uint8_t> digest(32);
  if (crypto_generichash(digest.data(), digest.size(), input.data(), input.size(), nullptr, 0) != 0) {
    return Error("BLAKE2b body hash failed");
  }
  return digest;
}

Roe<std::vector<uint8_t>> EnvelopeSigner::BuildSignBytes(const RelayEnvelope& envelope) {
  if (envelope.envelope_version != kRelayEnvelopeVersion) {
    return Error("Unsupported envelope version");
  }
  auto body_hash = BodyHash(envelope.body);
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
  }

  const std::string packed = oss.str();
  return std::vector<uint8_t>(packed.begin(), packed.end());
}

Roe<bool> EnvelopeSigner::Verify(const RelayEnvelope& envelope, const std::string& public_key_b64) {
  if (envelope.signature.empty()) {
    return Error("Missing envelope signature");
  }
  auto sign_bytes = BuildSignBytes(envelope);
  if (!sign_bytes) {
    return sign_bytes.error();
  }
  const auto public_key = Ed25519Signer::FromBase64(public_key_b64);
  if (!public_key) {
    return public_key.error();
  }
  return Ed25519Signer::Verify(std::string(sign_bytes->begin(), sign_bytes->end()), envelope.signature,
                               *public_key);
}

} // namespace pbr
