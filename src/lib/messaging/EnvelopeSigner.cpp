#include "lib/messaging/EnvelopeSigner.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"

#include "common/Serialize.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <set>
#include <cstring>
#include <sstream>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr std::array<char, 34> kSignDomain = {
    'p', 'p', '-', 'b', 'r', 'o', 'w', 's', 'e', 'r', ':', 'r', 'e', 'l', 'a', 'y', '-',
    'e', 'n', 'v', 'e', 'l', 'o', 'p', 'e', '-', 's', 'i', 'g', 'n', '-', 'v', '3', '\0'};

constexpr uint8_t kSignVersion = 3;
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
  const std::vector<RelayCopy> copies = RelayCopiesOf(envelope);
  if (copies.empty() || copies.size() > kMaxRelayCopies) {
    return Error("An envelope carries 1 to 8 copies");
  }
  std::set<std::string> devices;
  for (const RelayCopy& copy : copies) {
    if (copy.payload_b64.empty()) {
      return Error("A copy without a payload");
    }
    if (!devices.insert(copy.recipient_device).second) {
      return Error("Two copies for one device");
    }
  }
  if (envelope.route.kind == kRelayRouteGroup) {
    if (!envelope.route.group_id || envelope.route.group_id->empty()) {
      return Error("Group envelope without group_id");
    }
    return {};
  }
  if (envelope.route.kind == kRelayRouteDirect || envelope.route.kind == kRelayRouteControl) {
    if (envelope.route.group_id) {
      return Error("Direct / control envelope with a group_id");
    }
    if (envelope.route.kind == kRelayRouteControl && (copies.size() != 1 || !copies.front().recipient_device.empty())) {
      return Error("Control envelope must carry one account copy");
    }
    return {};
  }
  return Error("Unknown route kind");
}

Roe<std::vector<uint8_t>> EnvelopeSigner::CopyHash(const RelayCopy& copy) {
  EnsureSodiumInit();
  auto decoded = Base64Decode(copy.payload_b64);
  if (!decoded) {
    return decoded.error();
  }
  std::ostringstream oss;
  oss.put(static_cast<char>(kBodyKindE2e));
  {
    OutputArchive ar(oss);
    WireLenBytes blob{*decoded};
    ar & blob;
  }
  const std::string input = oss.str();
  std::vector<uint8_t> digest(32);
  if (crypto_generichash(digest.data(), digest.size(), reinterpret_cast<const unsigned char*>(input.data()),
                         input.size(), nullptr, 0) != 0) {
    return Error("BLAKE2b copy hash failed");
  }
  return digest;
}

Roe<std::vector<uint8_t>> EnvelopeSigner::BuildSignBytes(const RelayEnvelope& envelope) {
  if (envelope.envelope_version != kRelayEnvelopeVersion) {
    return Error("Unsupported envelope version");
  }
  if (auto shape = CheckBodyShape(envelope); !shape) {
    return shape.error();
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

  // Each copy (M022), by recipient device: who it is for, its numbering in that pair, the session it may
  // open, and exactly what that device decrypts. Copies for one device cannot be moved to another.
  const std::vector<RelayCopy> listed = RelayCopiesOf(envelope);
  std::vector<const RelayCopy*> copies;
  for (const RelayCopy& copy : listed) {
    copies.push_back(&copy);
  }
  std::sort(copies.begin(), copies.end(),
            [](const RelayCopy* a, const RelayCopy* b) { return a->recipient_device < b->recipient_device; });
  {
    OutputArchive ar(oss);
    WireLenUtf8 message_id{envelope.message_id};
    ar & message_id;
    WireLenUtf8 sender_contact_id{envelope.sender_contact_id};
    ar & sender_contact_id;
    WireLenUtf8 sender_relay_id{envelope.sender_relay_id};
    ar & sender_relay_id;
    WireLenUtf8 group_id{envelope.route.group_id.value_or(std::string{})};
    ar & group_id;
    WireLenUtf8 sender_device{envelope.sender_device};
    ar & sender_device;
  }
  AppendBigEndianUInt64(oss, copies.size());
  for (const RelayCopy* copy : copies) {
    auto hash = CopyHash(*copy);
    if (!hash) {
      return hash.error();
    }
    {
      OutputArchive ar(oss);
      WireLenUtf8 device{copy->recipient_device};
      ar & device;
    }
    AppendBigEndianUInt32(oss, copy->session_epoch);
    AppendBigEndianUInt64(oss, copy->sender_seq);
    {
      OutputArchive ar(oss);
      WireLenUtf8 key_init{copy->key_init_b64.value_or(std::string{})};
      ar & key_init;
    }
    oss.write(reinterpret_cast<const char*>(hash->data()), static_cast<std::streamsize>(hash->size()));
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
