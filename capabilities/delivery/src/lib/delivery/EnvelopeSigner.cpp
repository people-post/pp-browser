#include "lib/delivery/EnvelopeSigner.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/TypedBlob.h"
#include "foundation/error/AppError.h"

#include "common/Serialize.hpp"

#include <sodium.h>

#include <algorithm>
#include <set>
#include <sstream>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr uint8_t kBodyKindE2e = 0x02;

/** Envelope v4 is release 1's floor and only version. */
constexpr SignedVersions kEnvelopeVersions{.floor = kRelayEnvelopeVersion, .current = kRelayEnvelopeVersion};

/** One typed blob's id: known → nullopt; unknown → why; malformed → an error. */
Roe<std::optional<std::string>> UnknownAlgorithmOf(const pp::BlobKind kind, const std::string& blob_b64,
                                                   const std::string_view what) {
  const TypedRead read = ReadTypedBlobB64(kind, blob_b64, what);
  if (read.ok()) {
    return std::optional<std::string>{};
  }
  if (read.status == TypedBlobStatus::UnknownAlgorithm) {
    return std::optional<std::string>{read.detail};
  }
  return Error(read.detail);
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
    return 1; // always e2e_public
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
    return {};
  }
  // A control-lane record carries a copy per device of the peer's, or the account copy, as a direct one (M024).
  if (envelope.route.kind == kRelayRouteDirect || envelope.route.kind == kRelayRouteControl) {
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
  SignedBytesWriter out(kDomain, static_cast<uint8_t>(envelope.envelope_version));
  out.U8(RouteKindByte(envelope.route)).U8(ChannelByte(envelope.route)).I64(envelope.timestamp);
  out.Utf8(envelope.message_id).Utf8(envelope.sender_contact_id).Utf8(envelope.sender_device);

  // Each copy (M022), by recipient device: who it is for, its numbering in that pair, the session it may
  // open, and exactly what that device decrypts. Copies for one device cannot be moved to another.
  const std::vector<RelayCopy> listed = RelayCopiesOf(envelope);
  std::vector<const RelayCopy*> copies;
  for (const RelayCopy& copy : listed) {
    copies.push_back(&copy);
  }
  std::sort(copies.begin(), copies.end(),
            [](const RelayCopy* a, const RelayCopy* b) { return a->recipient_device < b->recipient_device; });
  out.U64(copies.size());
  for (const RelayCopy* copy : copies) {
    auto hash = CopyHash(*copy);
    if (!hash) {
      return hash.error();
    }
    out.Utf8(copy->recipient_device).U32(copy->session_epoch).U64(copy->sender_seq);
    out.Utf8(copy->key_init_b64.value_or(std::string{})).Raw(*hash);
  }
  return out.Finish(envelope.ext);
}

Roe<void> EnvelopeSigner::Sign(RelayEnvelope& envelope, const RawSignFn& sign) {
  auto bytes = BuildSignBytes(envelope);
  if (!bytes) {
    return bytes.error();
  }
  auto signature = SignTypedB64(sign, *bytes);
  if (!signature) {
    return signature.error();
  }
  envelope.signature = std::move(*signature);
  return {};
}

Roe<std::optional<std::string>> EnvelopeSigner::UnknownAlgorithm(const RelayEnvelope& envelope) {
  if (!envelope.signature.empty()) {
    auto signature = UnknownAlgorithmOf(pp::BlobKind::kSignature, envelope.signature, "envelope signature");
    if (!signature || *signature) {
      return signature;
    }
  }
  for (const RelayCopy& copy : RelayCopiesOf(envelope)) {
    if (copy.key_init_b64) {
      auto key_init = UnknownAlgorithmOf(pp::BlobKind::kKemCiphertext, *copy.key_init_b64, "copy key_init");
      if (!key_init || *key_init) {
        return key_init;
      }
    }
    auto payload = UnknownAlgorithmOf(pp::BlobKind::kAeadSealed, copy.payload_b64, "copy payload");
    if (!payload || *payload) {
      return payload;
    }
  }
  return std::optional<std::string>{};
}

SignedReceipt EnvelopeSigner::Receive(const RelayEnvelope& envelope, const std::string& device_key_b64) {
  TypedRead key;
  TypedRead signature;
  const auto parse = [&]() -> Roe<void> {
    if (envelope.signature.empty()) {
      return Error("no signature");
    }
    return CheckBodyShape(envelope);
  };
  const auto algorithms = [&]() -> Roe<void> {
    auto unknown = UnknownAlgorithm(envelope); // the copies' key_init and payload too
    if (!unknown) {
      return unknown.error();
    }
    if (*unknown) {
      return AppError::NewerFormat(**unknown);
    }
    signature = ReadTypedBlobB64(pp::BlobKind::kSignature, envelope.signature, "envelope signature");
    if (!signature.ok()) {
      return signature.ToError();
    }
    key = ReadTypedBlobB64(pp::BlobKind::kSignaturePublicKey, device_key_b64, "sending device key");
    if (!key.ok()) {
      return key.ToError();
    }
    return {};
  };
  const auto verify = [&]() -> Roe<void> {
    auto bytes = BuildSignBytes(envelope);
    if (!bytes) {
      return bytes.error();
    }
    return VerifyTyped(key, *bytes, signature);
  };
  return ReceiveSigned(SignedReceiveSteps{.what = "envelope",
                                          .outside_version = envelope.envelope_version,
                                          .versions = kEnvelopeVersions,
                                          .parse = parse,
                                          .algorithms = algorithms,
                                          .verify = verify,
                                          .ext = &envelope.ext});
}

} // namespace pbr
