#include "foundation/crypto/GraceUnlock.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/SealedDek.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <sodium.h>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr uint8_t kEntryVersion = 1;
constexpr size_t kGraceKeySize = SealedDek::kKeySize;
constexpr size_t kBootIdSize = 16;
constexpr size_t kEntrySize = 1 + kGraceKeySize + kBootIdSize + 8;
constexpr char kEntrySuffix[] = "/unlock_grace";
constexpr char kFileName[] = "grace.bin";

/** The store entry, decoded. K_grace is wiped when it goes out of scope. */
struct Entry {
  ByteVector key;
  std::array<uint8_t, kBootIdSize> boot_id{};
  int64_t expires_mono = 0;

  Entry() = default;
  Entry(const Entry&) = delete;
  Entry& operator=(const Entry&) = delete;
  Entry(Entry&&) = default;
  Entry& operator=(Entry&&) = default;
  ~Entry() {
    if (!key.empty()) {
      sodium_memzero(key.data(), key.size());
    }
  }
};

ByteVector EncodeEntry(const ByteVector& key, const std::array<uint8_t, kBootIdSize>& boot_id,
                       const int64_t expires_mono) {
  ByteVector out(kEntrySize);
  out[0] = kEntryVersion;
  std::copy(key.begin(), key.end(), out.begin() + 1);
  std::copy(boot_id.begin(), boot_id.end(), out.begin() + 1 + kGraceKeySize);
  const auto value = static_cast<uint64_t>(expires_mono);
  for (size_t i = 0; i < 8; ++i) {
    out[1 + kGraceKeySize + kBootIdSize + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xffu);
  }
  return out;
}

std::optional<Entry> DecodeEntry(const ByteVector& bytes) {
  if (bytes.size() != kEntrySize || bytes[0] != kEntryVersion) {
    return std::nullopt;
  }
  Entry entry;
  entry.key.assign(bytes.begin() + 1, bytes.begin() + 1 + kGraceKeySize);
  std::copy(bytes.begin() + 1 + kGraceKeySize, bytes.begin() + 1 + kGraceKeySize + kBootIdSize, entry.boot_id.begin());
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(bytes[1 + kGraceKeySize + kBootIdSize + i]) << (8 * i);
  }
  entry.expires_mono = static_cast<int64_t>(value);
  return entry;
}

void Wipe(ByteVector& bytes) {
  if (!bytes.empty()) {
    sodium_memzero(bytes.data(), bytes.size());
  }
}

// See GraceClock: without a clock no window can be measured, so none is opened, kept or renewed.
bool HasClock(const GraceClock& now) {
  return now.mono_seconds > 0;
}

int64_t ExpiryFrom(const GraceClock& now, const int minutes) {
  if (GraceIsAlways(minutes)) {
    return std::numeric_limits<int64_t>::max(); // end of this boot: the boot id is still checked (A015)
  }
  return now.mono_seconds + static_cast<int64_t>(minutes) * 60;
}

} // namespace

GraceUnlock::GraceUnlock(ISecureBlobStore& store, std::filesystem::path profile_dir, std::string profile_id)
    : store_(store), profile_dir_(std::move(profile_dir)), profile_id_(std::move(profile_id)) {}

bool GraceUnlock::Supported() const {
  return store_.Supported();
}

std::string GraceUnlock::EntryName() const {
  return profile_id_ + kEntrySuffix;
}

std::filesystem::path GraceUnlock::FilePath() const {
  return profile_dir_ / kFileName;
}

Roe<GraceUnlock::Result> GraceUnlock::TryUnlock(const GraceClock& now) {
  Result result;
  if (!Supported() || !HasClock(now)) {
    return result; // Missing, nothing read or deleted
  }
  auto looked = store_.Lookup(EntryName());
  if (!looked || (looked->status != SecureBlobLookup::Status::Found &&
                  looked->status != SecureBlobLookup::Status::Missing)) {
    // Locked device or an unknown store error: keep both pieces, the caller tries again later.
    result.status = Status::Unavailable;
    return result;
  }
  if (looked->status == SecureBlobLookup::Status::Missing) {
    Forget();
    return result;
  }
  auto entry = DecodeEntry(looked->bytes);
  Wipe(looked->bytes);
  if (!entry) {
    Forget();
    return result;
  }
  if (entry->expires_mono <= now.mono_seconds || entry->boot_id != now.boot_id) {
    Forget();
    result.status = Status::Expired;
    return result;
  }

  auto dek = SealedDek::OpenFile(FilePath(), entry->key, "grace|" + profile_id_);
  if (!dek) {
    Forget();
    return result;
  }
  result.status = Status::Found;
  result.dek = std::move(*dek);
  return result;
}

Roe<void> GraceUnlock::Remember(const ByteVector& dek, const int minutes, const GraceClock& now) {
  if (!Supported()) {
    return {};
  }
  if (!GraceIsOn(minutes)) {
    Forget();
    return {};
  }
  if (!HasClock(now)) {
    return {};
  }
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size");
  }
  ByteVector key = SealedDek::NewKey();
  if (auto sealed = SealedDek::SealFile(FilePath(), key, "grace|" + profile_id_, dek); !sealed) {
    Wipe(key);
    return sealed.error();
  }

  ByteVector entry = EncodeEntry(key, now.boot_id, ExpiryFrom(now, minutes));
  Wipe(key);
  auto stored = store_.Write(EntryName(), entry, SecureBlobAccess::kWhenUnlocked);
  Wipe(entry);
  if (!stored) {
    // The file alone (or with an older entry) is no window: leave neither piece behind.
    Forget();
    return stored.error();
  }
  return {};
}

Roe<void> GraceUnlock::Touch(const int minutes, const GraceClock& now) {
  if (!Supported()) {
    return {};
  }
  if (!GraceIsOn(minutes)) {
    Forget();
    return {};
  }
  if (!HasClock(now)) {
    return {};
  }
  auto looked = store_.Lookup(EntryName());
  if (!looked || looked->status != SecureBlobLookup::Status::Found) {
    return {}; // Missing stays missing (no resurrection); Unavailable is skipped
  }
  auto entry = DecodeEntry(looked->bytes);
  Wipe(looked->bytes);
  if (!entry) {
    return {};
  }
  // Touch runs only while the app is in the foreground, so the user is here now: that is the last use the window
  // counts from, even when the entry had already run out or comes from an earlier boot (same K_grace, new clock).
  ByteVector bytes = EncodeEntry(entry->key, now.boot_id, ExpiryFrom(now, minutes));
  auto stored = store_.Write(EntryName(), bytes, SecureBlobAccess::kWhenUnlocked);
  Wipe(bytes);
  if (!stored) {
    return stored.error();
  }
  return {};
}

void GraceUnlock::Forget() {
  if (!Supported()) {
    return;
  }
  SealedDek::Forget(store_, EntryName(), FilePath());
}

} // namespace pbr
