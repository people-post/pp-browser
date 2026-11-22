#include "foundation/crypto/SignedReceive.h"

#include "foundation/error/AppError.h"

#include <algorithm>
#include <limits>

namespace pbr {

namespace {

SignedReceipt Decided(const SignedReceiveSteps& steps, const SignedVerdict verdict, const std::string& why) {
  SignedReceipt receipt;
  receipt.verdict = verdict;
  receipt.version = steps.outside_version.value_or(0);
  receipt.detail = std::string(steps.what) + " v" + std::to_string(receipt.version) + ": " + why;
  return receipt;
}

/** A step before the signature: a NewerFormat error is "update needed", any other this step's refusal. */
std::optional<SignedReceipt> RunBeforeVerify(const SignedReceiveSteps& steps, const std::function<Roe<void>()>& step,
                                             const SignedVerdict refusal) {
  if (!step) {
    return std::nullopt;
  }
  auto result = step();
  if (result) {
    return std::nullopt;
  }
  const bool newer = AppError::IsNewerFormat(result.error());
  return Decided(steps, newer ? SignedVerdict::NeedsUpdate : refusal, result.error().message);
}

} // namespace

std::string_view SignedVerdictName(const SignedVerdict verdict) {
  switch (verdict) {
    case SignedVerdict::Accepted:
      return "accepted";
    case SignedVerdict::Older:
      return "older";
    case SignedVerdict::NeedsUpdate:
      return "needs_update";
    case SignedVerdict::Malformed:
      return "malformed";
    case SignedVerdict::Forged:
      return "forged";
  }
  return "malformed";
}

Error SignedReceipt::ToError() const {
  if (verdict == SignedVerdict::NeedsUpdate) {
    return AppError::NewerFormat(detail);
  }
  return Error(detail);
}

Roe<void> SignedReceipt::ToRoe() const {
  if (accepted()) {
    return {};
  }
  return ToError();
}

SignedReceipt ReceiveSigned(const SignedReceiveSteps& steps) {
  // 1. The outside version, before anything is parsed or verified.
  if (!steps.outside_version) {
    return Decided(steps, SignedVerdict::Older, "no version");
  }
  const int64_t version = *steps.outside_version;
  if (version < steps.versions.floor) {
    return Decided(steps, SignedVerdict::Older, "below the floor " + std::to_string(steps.versions.floor));
  }
  if (version > steps.versions.current) {
    return Decided(steps, SignedVerdict::NeedsUpdate, "newer than " + std::to_string(steps.versions.current));
  }
  // 2. Parse; 3. the algorithm ids.
  if (auto refused = RunBeforeVerify(steps, steps.parse, SignedVerdict::Malformed)) {
    return *refused;
  }
  if (auto refused = RunBeforeVerify(steps, steps.algorithms, SignedVerdict::Malformed)) {
    return *refused;
  }
  // 4. The signature over the rebuilt bytes, the extension area included.
  if (!steps.verify) {
    return Decided(steps, SignedVerdict::Forged, "nothing verifies it");
  }
  if (auto verified = steps.verify(); !verified) {
    return Decided(steps, SignedVerdict::Forged, verified.error().message);
  }
  // 5. The extension area, now that it is known to be the signer's.
  if (steps.ext != nullptr) {
    if (const auto unknown = steps.ext->FirstUnknownCritical(steps.understood_critical)) {
      return Decided(steps, SignedVerdict::NeedsUpdate, "unknown critical extension " + std::to_string(*unknown));
    }
  }
  SignedReceipt accepted;
  accepted.verdict = SignedVerdict::Accepted;
  accepted.version = version;
  return accepted;
}

std::optional<int64_t> OutsideVersionOfJson(const pp::common::Object& object, const std::string& key) {
  if (const auto signed_value = object.getIf<int64_t>(key)) {
    return signed_value;
  }
  // Built in memory with a uint64_t (parsed JSON holds int64_t): a version past int64's range is "newer" anyway.
  if (const auto unsigned_value = object.getIf<uint64_t>(key)) {
    return static_cast<int64_t>(std::min<uint64_t>(*unsigned_value, std::numeric_limits<int64_t>::max()));
  }
  return std::nullopt;
}

std::optional<int64_t> OutsideVersionOfBlob(const std::span<const uint8_t> blob) {
  if (blob.empty()) {
    return std::nullopt;
  }
  return blob[0];
}

} // namespace pbr
