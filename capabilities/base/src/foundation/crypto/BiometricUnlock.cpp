#include "foundation/crypto/BiometricUnlock.h"

#include "foundation/crypto/SealedDek.h"

#include <system_error>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr char kEntrySuffix[] = "/unlock_biometric";
constexpr char kFileName[] = "biometric.bin";

BiometricUnlock::Status StatusOf(const SealedDek::Status status) {
  switch (status) {
  case SealedDek::Status::Found:
    return BiometricUnlock::Status::Found;
  case SealedDek::Status::Missing:
    break;
  case SealedDek::Status::Unavailable:
    return BiometricUnlock::Status::Unavailable;
  case SealedDek::Status::Canceled:
    return BiometricUnlock::Status::Canceled;
  }
  return BiometricUnlock::Status::Missing;
}

} // namespace

BiometricUnlock::BiometricUnlock(ISecureBlobStore& store, std::filesystem::path profile_dir, std::string profile_id)
    : store_(store), profile_dir_(std::move(profile_dir)), profile_id_(std::move(profile_id)) {}

bool BiometricUnlock::Supported() const {
  return store_.Supports(SecureBlobAccess::kBiometryCurrentSet);
}

bool BiometricUnlock::Enabled() const {
  std::error_code ec;
  return std::filesystem::exists(FilePath(), ec) && !ec;
}

std::string BiometricUnlock::EntryName() const {
  return profile_id_ + kEntrySuffix;
}

std::filesystem::path BiometricUnlock::FilePath() const {
  return profile_dir_ / kFileName;
}

Roe<BiometricUnlock::Result> BiometricUnlock::TryUnlock(const std::string& prompt) {
  Result result;
  if (!Supported()) {
    return result;
  }
  auto opened = SealedDek::Open(store_, EntryName(), FilePath(), "biometric|" + profile_id_, prompt);
  if (!opened) {
    return opened.error();
  }
  result.status = StatusOf(opened->status);
  result.dek = std::move(opened->dek);
  result.code = opened->code;
  return result;
}

Roe<void> BiometricUnlock::Enable(const ByteVector& dek) {
  if (!Supported()) {
    return Error("Biometric unlock is not supported on this device");
  }
  return SealedDek::Seal(store_, EntryName(), FilePath(), "biometric|" + profile_id_, dek,
                         SecureBlobAccess::kBiometryCurrentSet);
}

void BiometricUnlock::Disable() {
  SealedDek::Forget(store_, EntryName(), FilePath());
}

} // namespace pbr
