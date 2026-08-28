#include "base/crypto/CanonicalAad.h"

#include "base/crypto/CryptoConstants.h"

#include "common/Serialize.hpp"

#include <sstream>
#include "common/PbrCompat.h"

namespace pbr {

Roe<ByteVector> CanonicalAad::Build(const AadFields& fields) {
  std::ostringstream oss;
  OutputArchive ar(oss);
  ar & kAadVersion;
  const uint8_t channel = static_cast<uint8_t>(fields.channel);
  ar & channel;
  WireLenUtf8 peer{fields.peer_contact_id};
  ar & peer;
  WireLenUtf8 message_id{fields.message_id};
  ar & message_id;
  WireLenUtf8 sender{fields.sender_contact_id};
  ar & sender;
  ar & fields.sender_seq;
  ar & fields.session_epoch;
  ar & fields.timestamp;
  const std::string data = oss.str();
  return ByteVector(data.begin(), data.end());
}

Roe<AadFields> CanonicalAad::Parse(const ByteVector& bytes) {
  const std::string data(bytes.begin(), bytes.end());
  std::istringstream iss(data);
  InputArchive ar(iss);

  AadFields fields;
  uint8_t version = 0;
  ar & version;
  if (ar.failed() || version != kAadVersion) {
    return Error("Unsupported AAD version");
  }
  uint8_t channel_raw = 0;
  ar & channel_raw;
  if (channel_raw > static_cast<uint8_t>(CryptoChannel::E2ePublic)) {
    return Error("Unknown AAD channel");
  }
  fields.channel = static_cast<CryptoChannel>(channel_raw);
  WireLenUtf8 peer;
  ar & peer;
  fields.peer_contact_id = peer.value;
  WireLenUtf8 message_id;
  ar & message_id;
  fields.message_id = message_id.value;
  WireLenUtf8 sender;
  ar & sender;
  fields.sender_contact_id = sender.value;
  ar & fields.sender_seq;
  ar & fields.session_epoch;
  ar & fields.timestamp;
  if (ar.failed() || !ar.exactEnd()) {
    return Error("Malformed AAD");
  }
  return fields;
}

} // namespace pbr
