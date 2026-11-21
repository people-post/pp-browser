#include "lib/net/RelayCertificate.h"

#include "lib/net/RelayOrigins.h"
#include "lib/people/AccountCard.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/SignedReceive.h"
#include "foundation/crypto/TypedBlob.h"
#include "foundation/error/AppError.h"

#include "crypto/PeerId.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace pbr {

namespace {

using pp::BlobKind;

std::string LowerAscii(const std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

/** Writes a blob's fields in the pp binary wire profile (big endian, u64 lengths): the parts that are not signed bytes. */
class BlobWriter {
public:
  BlobWriter& U8(const uint8_t value) {
    out_.push_back(value);
    return *this;
  }
  BlobWriter& U64(const uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      out_.push_back(static_cast<uint8_t>(value >> shift));
    }
    return *this;
  }
  BlobWriter& I64(const int64_t value) { return U64(static_cast<uint64_t>(value)); }
  BlobWriter& Bytes(const std::span<const uint8_t> value) {
    U64(value.size());
    out_.insert(out_.end(), value.begin(), value.end());
    return *this;
  }
  BlobWriter& Utf8(const std::string_view value) {
    U64(value.size());
    out_.insert(out_.end(), value.begin(), value.end());
    return *this;
  }
  BlobWriter& Ext(const pp::ExtArea& ext) {
    ext.AppendTo(out_);
    return *this;
  }
  BlobWriter& Signatures(const std::vector<OperatorSignature>& signatures) {
    U8(static_cast<uint8_t>(signatures.size()));
    for (const OperatorSignature& signature : signatures) {
      U8(signature.signer_index);
      Bytes(signature.signature);
    }
    return *this;
  }
  ByteVector Take() { return std::move(out_); }

private:
  ByteVector out_;
};

ByteVector OperatorSetChangeBody(const OperatorSetChange& change) {
  return BlobWriter().U64(change.set_generation).Bytes(change.new_set.Encode()).Ext(change.ext).Take();
}

/** A set's fields as they read (bounds and order; the keys' algorithms are read after, step 3). */
Roe<OperatorSet> ReadOperatorSet(const std::span<const uint8_t> bytes, const std::string_view what) {
  SignedBlobReader in(bytes);
  OperatorSet set;
  set.threshold = in.U8();
  const uint8_t n = in.U8();
  for (uint8_t i = 0; in.ok() && i < n; ++i) {
    set.keys.push_back(in.Bytes());
  }
  if (!in.Done()) {
    return Error(std::string(what) + ": does not parse");
  }
  if (n == 0 || n > kMaxOperatorKeys || set.threshold < 1 || set.threshold > n) {
    return Error(std::string(what) + ": needs 1 ≤ threshold ≤ n ≤ 8");
  }
  for (size_t i = 1; i < set.keys.size(); ++i) {
    if (!(set.keys[i - 1] < set.keys[i])) {
      return Error(std::string(what) + ": keys not sorted by bytes, or a key twice");
    }
  }
  return set;
}

/** `count` signatures, each an index into a set of `set_size` keys, none twice. */
Roe<std::vector<OperatorSignature>> ReadSignatures(SignedBlobReader& in, const std::string_view what) {
  const uint8_t count = in.U8();
  std::vector<OperatorSignature> signatures;
  for (uint8_t i = 0; in.ok() && i < count; ++i) {
    OperatorSignature signature;
    signature.signer_index = in.U8();
    signature.signature = in.Bytes();
    signatures.push_back(std::move(signature));
  }
  if (!in.ok()) {
    return Error(std::string(what) + ": signatures do not parse");
  }
  return signatures;
}

Roe<void> CheckSignerIndices(const std::vector<OperatorSignature>& signatures, const OperatorSet& set,
                             const std::string_view what) {
  if (signatures.size() > set.keys.size()) {
    return Error(std::string(what) + ": more signatures than operators");
  }
  std::set<uint8_t> seen;
  for (const OperatorSignature& signature : signatures) {
    if (signature.signer_index >= set.keys.size()) {
      return Error(std::string(what) + ": a signer index outside the set");
    }
    if (!seen.insert(signature.signer_index).second) {
      return Error(std::string(what) + ": a signer index twice");
    }
  }
  return {};
}

Roe<OperatorSetChange> ReadOperatorSetChange(const std::span<const uint8_t> blob, const size_t position) {
  const std::string what = "operator-set change " + std::to_string(position + 1);
  SignedBlobReader in(blob);
  const uint8_t version = in.U8();
  if (!in.ok()) {
    return Error(what + ": empty");
  }
  if (version > kOperatorSetChangeVersion) {
    return AppError::NewerFormat(what + ": version " + std::to_string(version));
  }
  if (version < kOperatorSetChangeVersion) {
    return Error(what + ": version " + std::to_string(version) + " is older than this build reads");
  }
  const ByteVector body = in.Bytes();
  auto signatures = ReadSignatures(in, what);
  if (!signatures) {
    return signatures.error();
  }
  if (!in.Done()) {
    return Error(what + ": does not parse");
  }
  SignedBlobReader fields(body);
  OperatorSetChange change;
  change.set_generation = fields.U64();
  const ByteVector set = fields.Bytes();
  change.ext = fields.Ext();
  if (!fields.Done()) {
    return Error(what + ": body does not parse");
  }
  auto new_set = ReadOperatorSet(set, what + " set");
  if (!new_set) {
    return new_set.error();
  }
  change.new_set = std::move(new_set.value());
  change.signatures = std::move(signatures.value());
  return change;
}

struct ParsedCertificate {
  RelayCertificateBody body;
  std::vector<OperatorSignature> signatures;
};

Roe<ParsedCertificate> ReadCertificateFields(const std::span<const uint8_t> blob) {
  SignedBlobReader in(blob);
  (void)in.U8(); // the version: step 1 read it
  const ByteVector body = in.Bytes();
  auto signatures = ReadSignatures(in, "relay certificate");
  if (!signatures) {
    return signatures.error();
  }
  if (!in.Done()) {
    return Error("relay certificate: does not parse");
  }
  ParsedCertificate parsed;
  parsed.signatures = std::move(signatures.value());
  RelayCertificateBody& out = parsed.body;
  SignedBlobReader fields(body);
  const ByteVector founding = fields.Bytes();
  const uint64_t change_count = fields.U64();
  if (!fields.ok() || change_count > kMaxOperatorSetChanges) {
    return Error("relay certificate: more than " + std::to_string(kMaxOperatorSetChanges) + " set changes");
  }
  std::vector<ByteVector> changes;
  for (uint64_t i = 0; fields.ok() && i < change_count; ++i) {
    changes.push_back(fields.Bytes());
  }
  out.sequence = fields.U64();
  out.issued_at = fields.I64();
  out.expires_at = fields.I64();
  const uint64_t origin_count = fields.U64();
  if (!fields.ok() || origin_count < 1 || origin_count > kMaxCertificateOrigins) {
    return Error("relay certificate: needs 1 to 4 origins");
  }
  for (uint64_t i = 0; fields.ok() && i < origin_count; ++i) {
    out.origins.push_back(fields.Utf8());
  }
  out.transport_key = fields.Bytes();
  out.signing_key = fields.Bytes();
  out.ext = fields.Ext();
  if (!fields.Done()) {
    return Error("relay certificate: body does not parse");
  }
  auto founding_set = ReadOperatorSet(founding, "founding operator set");
  if (!founding_set) {
    return founding_set.error();
  }
  out.founding = std::move(founding_set.value());
  const OperatorSet* in_force = &out.founding;
  for (size_t i = 0; i < changes.size(); ++i) {
    auto change = ReadOperatorSetChange(changes[i], i);
    if (!change) {
      return change.error();
    }
    if (auto indices = CheckSignerIndices(change->signatures, *in_force, "operator-set change"); !indices) {
      return indices.error();
    }
    out.changes.push_back(std::move(change.value()));
    in_force = &out.changes.back().new_set;
  }
  if (auto indices = CheckSignerIndices(parsed.signatures, *in_force, "relay certificate"); !indices) {
    return indices.error();
  }
  return parsed;
}

const OperatorSet& SetInForce(const RelayCertificateBody& body) {
  return body.changes.empty() ? body.founding : body.changes.back().new_set;
}

Roe<void> ReadKeys(const OperatorSet& set, const std::string_view what) {
  for (const ByteVector& key : set.keys) {
    if (const TypedRead read = ReadTypedBlob(BlobKind::kSignaturePublicKey, key, what); !read.ok()) {
      return read.ToError();
    }
  }
  return {};
}

Roe<void> ReadSignatureBlobs(const std::vector<OperatorSignature>& signatures, const std::string_view what) {
  for (const OperatorSignature& signature : signatures) {
    if (const TypedRead read = ReadTypedBlob(BlobKind::kSignature, signature.signature, what); !read.ok()) {
      return read.ToError();
    }
  }
  return {};
}

/** `signatures` by `set` over `message`: at least its threshold, every one valid. */
Roe<void> VerifyBySet(const OperatorSet& set, const std::vector<OperatorSignature>& signatures,
                      const ByteVector& message, const std::string_view what) {
  if (signatures.size() < set.threshold) {
    return Error(std::string(what) + ": " + std::to_string(signatures.size()) + " signature(s), the set needs " +
                 std::to_string(set.threshold));
  }
  for (const OperatorSignature& signature : signatures) {
    const TypedRead key = ReadTypedBlob(BlobKind::kSignaturePublicKey, set.keys[signature.signer_index], what);
    const TypedRead typed = ReadTypedBlob(BlobKind::kSignature, signature.signature, what);
    if (auto verified = VerifyTyped(key, message, typed); !verified) {
      return Error(std::string(what) + ": operator " + std::to_string(signature.signer_index) +
                   "'s signature does not verify");
    }
  }
  return {};
}

RelayCertificateCheck Refused(const RelayCertificateVerdict verdict, std::string detail) {
  return RelayCertificateCheck{.verdict = verdict, .certificate = std::nullopt, .detail = std::move(detail)};
}

/** § 3.2's rules on a verified certificate (step 6). */
RelayCertificateCheck CheckSemantics(RelayCertificate certificate, const int64_t now_ms) {
  const RelayCertificateBody& body = certificate.body;
  if (body.sequence < 1) {
    return Refused(RelayCertificateVerdict::Invalid, "relay certificate: sequence 0");
  }
  std::set<std::string> origins;
  for (const std::string& origin : body.origins) {
    if (!IsCertificateOrigin(origin)) {
      return Refused(RelayCertificateVerdict::Invalid, "relay certificate: an origin outside the bounds: " + origin);
    }
    if (!origins.insert(RelayOriginKey(origin)).second) {
      return Refused(RelayCertificateVerdict::Invalid, "relay certificate: an origin twice: " + origin);
    }
  }
  if (body.transport_key == body.signing_key) {
    return Refused(RelayCertificateVerdict::Invalid, "relay certificate: transport and signing keys are the same");
  }
  std::vector<const OperatorSet*> sets{&body.founding};
  for (const OperatorSetChange& change : body.changes) {
    sets.push_back(&change.new_set);
  }
  for (const OperatorSet* set : sets) {
    if (set->IndexOf(body.transport_key) || set->IndexOf(body.signing_key)) {
      return Refused(RelayCertificateVerdict::Invalid, "relay certificate: a relay key is an operator key");
    }
  }
  const TypedRead transport = ReadTypedBlob(BlobKind::kSignaturePublicKey, body.transport_key, "transport key");
  auto peer_id = pp::PeerIdFromMlDsaPublicKey(transport.raw);
  if (!transport.ok() || !peer_id) {
    return Refused(RelayCertificateVerdict::Invalid, "relay certificate: the transport key has no PeerId");
  }
  if (body.expires_at != 0 && body.expires_at <= now_ms) {
    return Refused(RelayCertificateVerdict::Expired, "relay certificate " + std::to_string(body.sequence) +
                                                         " expired at " + std::to_string(body.expires_at));
  }
  certificate.audience_peer_id = std::move(peer_id.value());
  return RelayCertificateCheck{.verdict = RelayCertificateVerdict::Accepted, .certificate = std::move(certificate)};
}

} // namespace

ByteVector OperatorSet::Encode() const {
  BlobWriter out;
  out.U8(threshold).U8(static_cast<uint8_t>(keys.size()));
  for (const ByteVector& key : keys) {
    out.Bytes(key);
  }
  return out.Take();
}

std::optional<size_t> OperatorSet::IndexOf(const std::span<const uint8_t> typed_key) const {
  for (size_t i = 0; i < keys.size(); ++i) {
    if (std::equal(keys[i].begin(), keys[i].end(), typed_key.begin(), typed_key.end())) {
      return i;
    }
  }
  return std::nullopt;
}

Roe<OperatorSet> MakeOperatorSet(const uint8_t threshold, std::vector<ByteVector> typed_keys) {
  std::sort(typed_keys.begin(), typed_keys.end());
  if (typed_keys.empty() || typed_keys.size() > kMaxOperatorKeys) {
    return Error("operator set: 1 to 8 keys");
  }
  if (std::adjacent_find(typed_keys.begin(), typed_keys.end()) != typed_keys.end()) {
    return Error("operator set: a key twice");
  }
  if (threshold < 1 || threshold > typed_keys.size()) {
    return Error("operator set: the threshold must be 1 to the number of keys");
  }
  OperatorSet set{.threshold = threshold, .keys = std::move(typed_keys)};
  if (auto read = ReadKeys(set, "operator key"); !read) {
    return read.error();
  }
  return set;
}

std::string RelayCertificate::ToBase64() const {
  return Base64Encode(blob);
}

bool RelayCertificate::Lists(const std::string& origin) const {
  const std::string key = RelayOriginKey(origin);
  return !key.empty() && std::any_of(body.origins.begin(), body.origins.end(),
                                     [&key](const std::string& listed) { return RelayOriginKey(listed) == key; });
}

std::string_view RelayCertificateVerdictName(const RelayCertificateVerdict verdict) {
  switch (verdict) {
    case RelayCertificateVerdict::Accepted:
      return "accepted";
    case RelayCertificateVerdict::Older:
      return "older";
    case RelayCertificateVerdict::NeedsUpdate:
      return "needs_update";
    case RelayCertificateVerdict::Malformed:
      return "malformed";
    case RelayCertificateVerdict::Forged:
      return "forged";
    case RelayCertificateVerdict::OtherRelay:
      return "other_relay";
    case RelayCertificateVerdict::Expired:
      return "expired";
    case RelayCertificateVerdict::Invalid:
      return "invalid";
  }
  return "malformed";
}

Error RelayCertificateCheck::ToError() const {
  if (verdict == RelayCertificateVerdict::NeedsUpdate) {
    return AppError::NewerFormat(detail);
  }
  return AppError::Network(Err::Network::RelayIdentityUnavailable,
                           "relay_certificate_" + std::string(RelayCertificateVerdictName(verdict)) + ": " + detail);
}

RelayCertificateCheck ReceiveRelayCertificate(const std::span<const uint8_t> blob, const RelayId& pinned,
                                              const int64_t now_ms) {
  if (blob.size() > kMaxRelayCertificateBytes) {
    return Refused(RelayCertificateVerdict::Malformed, "relay certificate: larger than the bound");
  }
  ParsedCertificate parsed;
  RelayId relay_id;
  bool other_relay = false;
  const SignedReceiveSteps steps{
      .what = "relay certificate",
      .outside_version = OutsideVersionOfBlob(blob),
      .versions = {.floor = kRelayCertificateVersion, .current = kRelayCertificateVersion},
      .parse = [&]() -> Roe<void> {
        auto fields = ReadCertificateFields(blob);
        if (!fields) {
          return fields.error();
        }
        parsed = std::move(fields.value());
        return {};
      },
      .algorithms = [&]() -> Roe<void> {
        if (auto read = ReadKeys(parsed.body.founding, "operator key"); !read) {
          return read;
        }
        for (const OperatorSetChange& change : parsed.body.changes) {
          if (auto read = ReadKeys(change.new_set, "operator key"); !read) {
            return read;
          }
          if (auto read = ReadSignatureBlobs(change.signatures, "operator-set change signature"); !read) {
            return read;
          }
        }
        if (auto read = ReadSignatureBlobs(parsed.signatures, "relay certificate signature"); !read) {
          return read;
        }
        for (const ByteVector* key : {&parsed.body.transport_key, &parsed.body.signing_key}) {
          if (const TypedRead read = ReadTypedBlob(BlobKind::kSignaturePublicKey, *key, "relay key"); !read.ok()) {
            return read.ToError();
          }
        }
        return {};
      },
      .verify = [&]() -> Roe<void> {
        relay_id = parsed.body.founding.Id();
        if (!pinned.empty() && relay_id != pinned) {
          other_relay = true;
          return Error("relay certificate of " + relay_id.ToString() + ", not of " + pinned.ToString());
        }
        const OperatorSet* in_force = &parsed.body.founding;
        uint64_t generation = kFoundingSetGeneration;
        for (const OperatorSetChange& change : parsed.body.changes) {
          if (change.set_generation != generation + 1) {
            return Error("operator-set change: generation " + std::to_string(change.set_generation) +
                         " after generation " + std::to_string(generation));
          }
          if (auto verified = VerifyBySet(*in_force, change.signatures, OperatorSetChangeSignedBytes(relay_id, change),
                                          "operator-set change");
              !verified) {
            return verified;
          }
          in_force = &change.new_set;
          generation = change.set_generation;
        }
        return VerifyBySet(*in_force, parsed.signatures, RelayCertificateSignedBytes(parsed.body), "relay certificate");
      },
      .ext = &parsed.body.ext,
  };
  const SignedReceipt receipt = ReceiveSigned(steps);
  switch (receipt.verdict) {
    case SignedVerdict::Accepted:
      break;
    case SignedVerdict::Older:
      return Refused(RelayCertificateVerdict::Older, receipt.detail);
    case SignedVerdict::NeedsUpdate:
      return Refused(RelayCertificateVerdict::NeedsUpdate, receipt.detail);
    case SignedVerdict::Malformed:
      return Refused(RelayCertificateVerdict::Malformed, receipt.detail);
    case SignedVerdict::Forged:
      return Refused(other_relay ? RelayCertificateVerdict::OtherRelay : RelayCertificateVerdict::Forged,
                     receipt.detail);
  }
  // Step 5 for the set changes' own areas: they verified with the certificate.
  for (const OperatorSetChange& change : parsed.body.changes) {
    if (const auto unknown = change.ext.FirstUnknownCritical({})) {
      return Refused(RelayCertificateVerdict::NeedsUpdate,
                     "operator-set change: unknown critical extension " + std::to_string(*unknown));
    }
  }
  RelayCertificate certificate;
  certificate.set_in_force = SetInForce(parsed.body);
  certificate.body = std::move(parsed.body);
  certificate.signatures = std::move(parsed.signatures);
  certificate.blob.assign(blob.begin(), blob.end());
  certificate.relay_id = relay_id;
  return CheckSemantics(std::move(certificate), now_ms);
}

RelayCertificateCheck ReceiveRelayCertificateB64(const std::string& certificate_b64, const RelayId& pinned,
                                                 const int64_t now_ms) {
  if (certificate_b64.size() > kMaxRelayCertificateBytes * 4 / 3 + 4) {
    return Refused(RelayCertificateVerdict::Malformed, "relay certificate: larger than the bound");
  }
  const auto blob = Base64Decode(certificate_b64);
  if (!blob || blob.value().empty()) {
    return Refused(RelayCertificateVerdict::Malformed, "relay certificate: not base64");
  }
  return ReceiveRelayCertificate(blob.value(), pinned, now_ms);
}

bool IsCertificateOrigin(const std::string& origin) {
  constexpr std::string_view kHttp = "http://";
  const bool http =
      origin.size() > kHttp.size() && LowerAscii(std::string_view(origin).substr(0, kHttp.size())) == kHttp;
  // Plain http (the local stack's relay, the lab's): the card's bounds on its https spelling.
  return IsCardRelayOrigin(http ? "https://" + origin.substr(kHttp.size()) : origin);
}

std::string_view RelayCertificateOfferName(const RelayCertificateOffer offer) {
  switch (offer) {
    case RelayCertificateOffer::Adopted:
      return "adopted";
    case RelayCertificateOffer::Same:
      return "same";
    case RelayCertificateOffer::Lower:
      return "lower";
    case RelayCertificateOffer::Conflict:
      return "conflict";
  }
  return "conflict";
}

RelayCertificateOffer CompareRelayCertificates(const RelayCertificate* held, const RelayCertificate& offered) {
  if (held == nullptr || offered.body.sequence > held->body.sequence) {
    return RelayCertificateOffer::Adopted;
  }
  if (offered.body.sequence < held->body.sequence) {
    return RelayCertificateOffer::Lower;
  }
  return offered.blob == held->blob ? RelayCertificateOffer::Same : RelayCertificateOffer::Conflict;
}

ByteVector RelayCertificateSignedBytes(const RelayCertificateBody& body) {
  SignedBytesWriter out(kRelayCertificateDomain, kRelayCertificateVersion);
  out.Bytes(body.founding.Encode()).U64(body.changes.size());
  for (const OperatorSetChange& change : body.changes) {
    out.Bytes(EncodeOperatorSetChange(change));
  }
  out.U64(body.sequence).I64(body.issued_at).I64(body.expires_at).U64(body.origins.size());
  for (const std::string& origin : body.origins) {
    out.Utf8(origin);
  }
  return out.Typed(body.transport_key).Typed(body.signing_key).Finish(body.ext);
}

ByteVector OperatorSetChangeSignedBytes(const RelayId& relay_id, const OperatorSetChange& change) {
  return SignedBytesWriter(kOperatorSetChangeDomain, kOperatorSetChangeVersion)
      .Raw(relay_id.raw())
      .U64(change.set_generation)
      .Bytes(change.new_set.Encode())
      .Finish(change.ext);
}

ByteVector EncodeRelayCertificateBody(const RelayCertificateBody& body) {
  BlobWriter out;
  out.Bytes(body.founding.Encode()).U64(body.changes.size());
  for (const OperatorSetChange& change : body.changes) {
    out.Bytes(EncodeOperatorSetChange(change));
  }
  out.U64(body.sequence).I64(body.issued_at).I64(body.expires_at).U64(body.origins.size());
  for (const std::string& origin : body.origins) {
    out.Utf8(origin);
  }
  return out.Bytes(body.transport_key).Bytes(body.signing_key).Ext(body.ext).Take();
}

ByteVector EncodeRelayCertificate(const RelayCertificateBody& body, const std::vector<OperatorSignature>& signatures,
                                  const uint8_t version) {
  return BlobWriter().U8(version).Bytes(EncodeRelayCertificateBody(body)).Signatures(signatures).Take();
}

ByteVector EncodeOperatorSetChange(const OperatorSetChange& change, const uint8_t version) {
  return BlobWriter().U8(version).Bytes(OperatorSetChangeBody(change)).Signatures(change.signatures).Take();
}

Roe<OperatorSetChange> SignOperatorSetChange(const RelayId& relay_id, OperatorSetChange change,
                                             const std::vector<OperatorSigner>& signers) {
  change.signatures.clear();
  const ByteVector message = OperatorSetChangeSignedBytes(relay_id, change);
  for (const OperatorSigner& signer : signers) {
    auto signature = SignTyped(signer.secret_key, message);
    if (!signature) {
      return signature.error();
    }
    change.signatures.push_back(OperatorSignature{.signer_index = signer.signer_index, .signature = *signature});
  }
  return change;
}

Roe<ByteVector> SignRelayCertificate(const RelayCertificateBody& body, const std::vector<OperatorSigner>& signers) {
  const ByteVector message = RelayCertificateSignedBytes(body);
  std::vector<OperatorSignature> signatures;
  for (const OperatorSigner& signer : signers) {
    auto signature = SignTyped(signer.secret_key, message);
    if (!signature) {
      return signature.error();
    }
    signatures.push_back(OperatorSignature{.signer_index = signer.signer_index, .signature = *signature});
  }
  return EncodeRelayCertificate(body, signatures);
}

} // namespace pbr
