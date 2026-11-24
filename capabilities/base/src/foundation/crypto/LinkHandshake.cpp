#include "foundation/crypto/LinkHandshake.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MessageCipher.h"
#include "foundation/crypto/RecoverySeed.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/TypedBlob.h"
#include "foundation/error/AppError.h"

#include "crypto/MlKem.h"

#include <sodium.h>

#include <cstring>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

/** Hash tags and KDF labels, not signing domains (SIGNED_EXTENSIONS § Signed bytes): they keep their `-vN`. */
constexpr char kCodeTag[] = "pp-browser-link-code-v1";
constexpr char kReplyAadTag[] = "pp-browser-link-reply-v1";
constexpr char kReplyHkdfSalt[] = "pp-browser-link-v1";
constexpr char kReplyKeyInfo[] = "link-reply-key";
constexpr size_t kReplyKeyBytes = 32;
constexpr size_t kSeedBytes = 32;
constexpr uint32_t kCodeModulus = 10000;

void Zero(ByteVector& bytes) {
  if (!bytes.empty()) {
    sodium_memzero(bytes.data(), bytes.size());
  }
  bytes.clear();
}

/** Big-endian fields into a buffer the caller zeroes (the bundle holds the seed: no stream copies). */
class PlainWriter {
public:
  explicit PlainWriter(ByteVector& out) : out_(out) {}
  void U8(const uint8_t value) { out_.push_back(value); }
  void U64(const uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      out_.push_back(static_cast<uint8_t>((value >> shift) & 0xffU));
    }
  }
  void I64(const int64_t value) { U64(static_cast<uint64_t>(value)); }
  void Raw(const std::span<const uint8_t> bytes) { out_.insert(out_.end(), bytes.begin(), bytes.end()); }
  void LenBytes(const std::span<const uint8_t> bytes) {
    U64(bytes.size());
    Raw(bytes);
  }
  void LenUtf8(const std::string_view text) {
    U64(text.size());
    out_.insert(out_.end(), text.begin(), text.end());
  }

private:
  ByteVector& out_;
};

Error Unreadable(const std::string& detail) {
  return MakeLinkError(LinkError::Unreadable, detail);
}

/** Step 1 for either payload: the first byte. */
Roe<void> CheckVersion(const std::span<const uint8_t> payload, const uint8_t current, const char* what) {
  if (payload.empty() || payload[0] == 0) {
    return Unreadable(std::string(what) + ": no version");
  }
  if (payload[0] > current) {
    return AppError::NewerFormat(std::string(what) + " version " + std::to_string(payload[0]));
  }
  return {};
}

/** Step 5: an unknown critical entry (none is known at version 1) is a newer app's. */
Roe<void> CheckExtensions(const pp::ExtArea& ext, const char* what) {
  if (ext.HasUnknownCritical({})) {
    return AppError::NewerFormat(std::string(what) + ": an extension this app does not know");
  }
  return {};
}

Roe<ByteVector> ReplyKey(const ByteVector& shared_secret) {
  EnsureSodiumInit();
  unsigned char prk[crypto_kdf_hkdf_sha256_KEYBYTES];
  if (crypto_kdf_hkdf_sha256_extract(prk, reinterpret_cast<const unsigned char*>(kReplyHkdfSalt),
                                     std::strlen(kReplyHkdfSalt), shared_secret.data(), shared_secret.size()) != 0) {
    return Error("link reply: HKDF extract failed");
  }
  ByteVector key(kReplyKeyBytes);
  const int expanded =
      crypto_kdf_hkdf_sha256_expand(key.data(), key.size(), kReplyKeyInfo, std::strlen(kReplyKeyInfo), prk);
  sodium_memzero(prk, sizeof(prk));
  if (expanded != 0) {
    return Error("link reply: HKDF expand failed");
  }
  return key;
}

ByteVector ReplyAad(const uint8_t version, const std::span<const uint8_t> offer,
                    const std::span<const uint8_t> typed_ciphertext) {
  ByteVector aad(kReplyAadTag, kReplyAadTag + std::strlen(kReplyAadTag));
  PlainWriter out(aad);
  out.U8(0);
  out.U8(version);
  out.LenBytes(offer);
  out.LenBytes(typed_ciphertext);
  return aad;
}

Roe<void> CheckRelay(const RelayAddress& relay) {
  if (relay.origin.empty() || relay.origin.size() > kMaxLinkRelayOriginBytes || relay.relay_id.empty()) {
    return Error("link bundle: a relay needs an origin of at most 128 bytes and a relay id");
  }
  return {};
}

ByteVector BundleBytes(const LinkBundle& bundle, const pp::ExtArea& ext) {
  ByteVector plain;
  PlainWriter out(plain);
  out.Raw(bundle.seed);
  out.U8(static_cast<uint8_t>(bundle.intent));
  out.I64(bundle.expires_at_ms);
  out.U8(static_cast<uint8_t>(bundle.relay ? 1 : 0));
  if (bundle.relay) {
    out.LenUtf8(bundle.relay->origin);
    out.Raw(bundle.relay->relay_id.raw());
  }
  ext.AppendTo(plain);
  return plain;
}

/** The bundle's fields from an opened reply (version 1). */
Roe<LinkBundle> ParseBundle(const ByteVector& plain) {
  SignedBlobReader in(plain);
  LinkBundle bundle;
  bundle.seed = in.Raw(kSeedBytes);
  const uint8_t intent = in.U8();
  bundle.expires_at_ms = in.I64();
  const uint8_t relays = in.U8();
  if (in.ok() && relays > 1) {
    return Unreadable("link bundle: more than one relay");
  }
  if (relays == 1) {
    std::string origin = in.Utf8();
    const ByteVector raw_id = in.Raw(kRelayIdBytes);
    if (in.ok()) {
      std::optional<RelayId> relay_id = RelayId::FromRaw(raw_id);
      if (!relay_id || origin.empty() || origin.size() > kMaxLinkRelayOriginBytes) {
        return Unreadable("link bundle: its relay is out of bounds");
      }
      bundle.relay = RelayAddress{.origin = std::move(origin), .relay_id = *relay_id};
    }
  }
  const pp::ExtArea ext = in.Ext();
  if (!in.Done()) {
    return Unreadable("link bundle: its fields do not parse, or bytes follow them");
  }
  if (auto known = CheckExtensions(ext, "link bundle"); !known) {
    return known.error();
  }
  if (intent != static_cast<uint8_t>(LinkIntent::Join)) {
    return AppError::NewerFormat("link bundle: intent " + std::to_string(intent));
  }
  bundle.intent = LinkIntent::Join;
  return bundle;
}

} // namespace

Error MakeLinkError(const LinkError code, const std::string& detail) {
  return Error::Make(kLinkErrorCategory, static_cast<int32_t>(code), detail);
}

bool IsLinkError(const Error& error, const LinkError code) {
  return error.category == kLinkErrorCategory && error.code == static_cast<int32_t>(code);
}

LinkOfferKeys::LinkOfferKeys(ByteVector public_key, ByteVector secret_key)
    : public_key_(std::move(public_key)), secret_key_(std::move(secret_key)) {}

LinkOfferKeys::~LinkOfferKeys() {
  Wipe();
}

LinkOfferKeys::LinkOfferKeys(LinkOfferKeys&& other) noexcept
    : public_key_(std::move(other.public_key_)), secret_key_(std::move(other.secret_key_)) {
  other.Wipe();
}

LinkOfferKeys& LinkOfferKeys::operator=(LinkOfferKeys&& other) noexcept {
  if (this != &other) {
    Wipe();
    public_key_ = std::move(other.public_key_);
    secret_key_ = std::move(other.secret_key_);
    other.Wipe();
  }
  return *this;
}

Roe<LinkOfferKeys> LinkOfferKeys::Generate() {
  auto keys = ::pp::MlKem::GenerateKeyPair();
  if (!keys) {
    return keys.error();
  }
  return LinkOfferKeys(std::move(keys->public_key), std::move(keys->private_key));
}

void LinkOfferKeys::Wipe() {
  Zero(secret_key_);
  public_key_.clear();
}

LinkBundle::~LinkBundle() {
  Zero(seed);
}

LinkBundle::LinkBundle(LinkBundle&& other) noexcept
    : seed(std::move(other.seed)), intent(other.intent), expires_at_ms(other.expires_at_ms),
      relay(std::move(other.relay)) {
  Zero(other.seed);
}

LinkBundle& LinkBundle::operator=(LinkBundle&& other) noexcept {
  if (this != &other) {
    Zero(seed);
    seed = std::move(other.seed);
    intent = other.intent;
    expires_at_ms = other.expires_at_ms;
    relay = std::move(other.relay);
    Zero(other.seed);
  }
  return *this;
}

Roe<ByteVector> LinkOfferBytes(const std::span<const uint8_t> kem_public_key, const pp::ExtArea& ext) {
  auto typed = TypedKemPublicKey(kem_public_key);
  if (!typed) {
    return typed.error();
  }
  ByteVector offer;
  PlainWriter out(offer);
  out.U8(kLinkOfferVersion);
  out.LenBytes(*typed);
  ext.AppendTo(offer);
  return offer;
}

Roe<ByteVector> ReadLinkOffer(const std::span<const uint8_t> offer) {
  if (offer.size() > kMaxLinkPayloadBytes) {
    return Unreadable("link offer: too long");
  }
  if (auto version = CheckVersion(offer, kLinkOfferVersion, "link offer"); !version) {
    return version.error();
  }
  SignedBlobReader in(offer);
  (void)in.U8(); // the version, read above
  const ByteVector typed_key = in.Bytes();
  const pp::ExtArea ext = in.Ext();
  if (!in.Done()) {
    return Unreadable("link offer: its fields do not parse, or bytes follow them");
  }
  TypedRead key = ReadTypedBlob(pp::BlobKind::kKemPublicKey, typed_key, "link offer key");
  if (!key.ok()) {
    return key.status == TypedBlobStatus::UnknownAlgorithm ? key.ToError() : Unreadable(key.detail);
  }
  if (auto known = CheckExtensions(ext, "link offer"); !known) {
    return known.error();
  }
  return std::move(key.raw);
}

std::string LinkConfirmationCode(const std::span<const uint8_t> offer) {
  ByteVector input(kCodeTag, kCodeTag + std::strlen(kCodeTag));
  input.push_back(0);
  input.insert(input.end(), offer.begin(), offer.end());
  const ByteVector hash = Blake2b256(input);
  const uint32_t first = (static_cast<uint32_t>(hash[0]) << 24) | (static_cast<uint32_t>(hash[1]) << 16) |
                         (static_cast<uint32_t>(hash[2]) << 8) | static_cast<uint32_t>(hash[3]);
  std::string code = std::to_string(first % kCodeModulus);
  return std::string(4 - code.size(), '0') + code;
}

Roe<ByteVector> SealLinkReply(const std::span<const uint8_t> offer, const LinkBundle& bundle, const pp::ExtArea& ext) {
  auto offer_key = ReadLinkOffer(offer);
  if (!offer_key) {
    return offer_key.error();
  }
  if (bundle.seed.size() != kSeedBytes) {
    return Error("link bundle: the recovery seed must be 32 bytes");
  }
  if (bundle.expires_at_ms <= 0) {
    return Error("link bundle: no expiry");
  }
  if (bundle.relay) {
    if (auto relay = CheckRelay(*bundle.relay); !relay) {
      return relay.error();
    }
  }
  auto encapsulated = ::pp::MlKem::Encapsulate(*offer_key);
  if (!encapsulated) {
    return encapsulated.error();
  }
  auto typed_ciphertext = TypedKemCiphertext(encapsulated->ciphertext);
  auto key = ReplyKey(encapsulated->shared_secret);
  Zero(encapsulated->shared_secret);
  if (!typed_ciphertext || !key) {
    return !typed_ciphertext ? typed_ciphertext.error() : key.error();
  }
  ByteVector plain = BundleBytes(bundle, ext);
  auto nonce = MessageCipher::GenerateNonce();
  auto sealed = nonce ? MessageCipher::Encrypt(*key, plain, ReplyAad(kLinkReplyVersion, offer, *typed_ciphertext), *nonce)
                      : Roe<EncryptedBlob>(nonce.error());
  Zero(plain);
  Zero(*key);
  if (!sealed) {
    return sealed.error();
  }
  ByteVector body = sealed->nonce;
  body.insert(body.end(), sealed->ciphertext.begin(), sealed->ciphertext.end());
  auto typed_sealed = WriteTypedBlob(pp::BlobKind::kAeadSealed, body);
  if (!typed_sealed) {
    return typed_sealed.error();
  }
  ByteVector reply;
  PlainWriter out(reply);
  out.U8(kLinkReplyVersion);
  out.LenBytes(*typed_ciphertext);
  out.LenBytes(*typed_sealed);
  return reply;
}

Roe<LinkBundle> OpenLinkReply(const std::span<const uint8_t> offer, const LinkOfferKeys& keys,
                              const std::span<const uint8_t> reply, const int64_t now_ms) {
  // 1. version
  if (reply.size() > kMaxLinkPayloadBytes) {
    return Unreadable("link reply: too long");
  }
  if (auto version = CheckVersion(reply, kLinkReplyVersion, "link reply"); !version) {
    return version.error();
  }
  if (keys.empty()) {
    return MakeLinkError(LinkError::Expired, "link reply: this device's one-time key is gone");
  }
  // 2. fields
  SignedBlobReader in(reply);
  (void)in.U8();
  const ByteVector typed_ciphertext = in.Bytes();
  const ByteVector typed_sealed = in.Bytes();
  if (!in.Done()) {
    return Unreadable("link reply: its fields do not parse, or bytes follow them");
  }
  // 3. algorithm ids
  TypedRead ciphertext = ReadTypedBlob(pp::BlobKind::kKemCiphertext, typed_ciphertext, "link reply ciphertext");
  if (!ciphertext.ok()) {
    return ciphertext.status == TypedBlobStatus::UnknownAlgorithm ? ciphertext.ToError()
                                                                   : Unreadable(ciphertext.detail);
  }
  TypedRead sealed = ReadTypedBlob(pp::BlobKind::kAeadSealed, typed_sealed, "link reply bundle");
  if (!sealed.ok()) {
    return sealed.status == TypedBlobStatus::UnknownAlgorithm ? sealed.ToError() : Unreadable(sealed.detail);
  }
  // 4. open: only this device's one-time key, for this offer (ML-KEM's implicit rejection gives a key that fails here)
  auto shared = ::pp::MlKem::Decapsulate(keys.secret_key(), ciphertext.raw);
  if (!shared) {
    return MakeLinkError(LinkError::NotForThisDevice, "link reply: " + shared.error().message);
  }
  auto key = ReplyKey(*shared);
  Zero(*shared);
  if (!key) {
    return key.error();
  }
  EncryptedBlob blob;
  blob.nonce.assign(sealed.raw.begin(), sealed.raw.begin() + kAeadNonceSize);
  blob.ciphertext.assign(sealed.raw.begin() + kAeadNonceSize, sealed.raw.end());
  auto plain = MessageCipher::Decrypt(*key, blob, ReplyAad(reply[0], offer, typed_ciphertext));
  Zero(*key);
  if (!plain) {
    return MakeLinkError(LinkError::NotForThisDevice, "link reply: it does not open with this device's key");
  }
  // 5. the bundle's fields and area (the sender's word only now that it opened)
  auto bundle = ParseBundle(*plain);
  Zero(*plain);
  if (!bundle) {
    return bundle.error();
  }
  // 6. freshness
  if (now_ms >= bundle->expires_at_ms) {
    return MakeLinkError(LinkError::Expired, "link reply: it has expired");
  }
  return bundle;
}

std::string LinkPayloadText(const std::string_view prefix, const std::span<const uint8_t> payload) {
  return std::string(prefix) + Base64UrlEncode(ByteVector(payload.begin(), payload.end()));
}

std::optional<ByteVector> FindLinkPayload(const std::string_view prefix, const std::string_view input) {
  if (const size_t at = input.find(prefix); at != std::string_view::npos) {
    std::string text;
    for (size_t i = at + prefix.size(); i < input.size() && text.size() <= kMaxLinkPayloadBytes * 2; ++i) {
      const char c = input[i];
      const bool b64 = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                       c == '_';
      if (!b64) {
        break;
      }
      text.push_back(c);
    }
    auto decoded = Base64UrlDecode(text);
    if (!decoded || decoded->empty()) {
      return std::nullopt;
    }
    return std::move(*decoded);
  }
  if (!input.empty() && static_cast<uint8_t>(input[0]) < kLinkPayloadVersionLimit &&
      input.size() <= kMaxLinkPayloadBytes) {
    return ByteVector(input.begin(), input.end());
  }
  return std::nullopt;
}

} // namespace pbr
