#include "base/net/RegistrationClientUtil.h"

#include "base/platform/os/OsTime.h"
#include "common/Utilities.h"
#include "base/net/RegistrationSignPayload.h"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <optional>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr int64_t kExpiringSoonWindowMs = 14LL * 24 * 60 * 60 * 1000;
constexpr const char kExpiredEpochIso[] = "1970-01-01T00:00:00.000Z";

/** Parse ISO-8601 timestamps like 2026-07-13T12:00:00Z / .123Z; returns nullopt on failure. */
std::optional<int64_t> ParseIso8601ToUnixMs(const std::string& iso) {
  if (iso.size() < 19) {
    return std::nullopt;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
    return std::nullopt;
  }

  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;
  tm.tm_isdst = 0;

  const time_t seconds = os::TimeGm(&tm);
  if (seconds == static_cast<time_t>(-1)) {
    return std::nullopt;
  }

  int64_t ms = static_cast<int64_t>(seconds) * 1000;
  const size_t dot = iso.find('.');
  if (dot != std::string::npos && dot + 1 < iso.size()) {
    int frac = 0;
    int digits = 0;
    for (size_t i = dot + 1; i < iso.size() && std::isdigit(static_cast<unsigned char>(iso[i])) && digits < 3; ++i) {
      frac = frac * 10 + (iso[i] - '0');
      ++digits;
    }
    while (digits > 0 && digits < 3) {
      frac *= 10;
      ++digits;
    }
    ms += frac;
  }
  return ms;
}

int64_t ResolveNowMs(int64_t now_ms) {
  return now_ms > 0 ? now_ms : util::NowUnixMs();
}

} // namespace

RegistrationStatus ClassifyRegistration(const LocalIdentity& identity, int64_t now_ms) {
  if (!identity.registered) {
    return RegistrationStatus::Unregistered;
  }
  const int64_t now = ResolveNowMs(now_ms);
  if (identity.registration_expires_at.empty()) {
    return RegistrationStatus::Expired;
  }
  const auto expires_ms = ParseIso8601ToUnixMs(identity.registration_expires_at);
  if (!expires_ms) {
    return RegistrationStatus::Expired;
  }
  if (*expires_ms <= now) {
    return RegistrationStatus::Expired;
  }
  if (*expires_ms - now <= kExpiringSoonWindowMs) {
    return RegistrationStatus::ExpiringSoon;
  }
  return RegistrationStatus::Active;
}

bool ShouldRenewRegistration(const LocalIdentity& identity, int64_t now_ms) {
  const RegistrationStatus status = ClassifyRegistration(identity, now_ms);
  return status == RegistrationStatus::ExpiringSoon || status == RegistrationStatus::Expired;
}

std::string RegistrationStatusLabel(RegistrationStatus status) {
  switch (status) {
  case RegistrationStatus::Unregistered:
    return "not registered";
  case RegistrationStatus::Active:
    return "active";
  case RegistrationStatus::ExpiringSoon:
    return "expiring soon";
  case RegistrationStatus::Expired:
    return "expired";
  }
  return "not registered";
}

std::string RegistrationActionLabel(RegistrationStatus status) {
  switch (status) {
  case RegistrationStatus::Unregistered:
    return "Register on network";
  case RegistrationStatus::Active:
  case RegistrationStatus::ExpiringSoon:
  case RegistrationStatus::Expired:
    return "Renew registration";
  }
  return "Register on network";
}

void ApplyRegistrationResult(LocalIdentity& identity, const RegistrationResult& result) {
  identity.registered = result.success;
  if (!result.relay_user_id.empty()) {
    identity.relay_user_id = result.relay_user_id;
  }
  if (!result.llm_api_key.empty()) {
    identity.brief_llm_api_key = result.llm_api_key;
    identity.brief_llm_guest_api_key.clear();
  }
  if (!result.expires_at.empty()) {
    identity.registration_expires_at = result.expires_at;
  }
  if (result.initiation_floor_present) {
    identity.initiation_floor = result.initiation_floor;
  }
}

void MarkRegistrationExpired(LocalIdentity& identity) {
  identity.registration_expires_at = kExpiredEpochIso;
}

Roe<RegistrationResult> FinishRegistrationWithIdentity(IRegistrationClient& registration, IdentityStore& identity,
                                                       const std::string& nickname,
                                                       const std::vector<std::string>& multiaddrs,
                                                       const RegistrationPublishOpts& publish) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  if (loaded->account_signing_public_key_b64.empty() || loaded->account_id.empty()) {
    return Error("account signing key / Account ID not set");
  }
  if (loaded->kem_public_key_b64.empty()) {
    return Error("kem_public_key_b64 not set");
  }

  auto start = registration.StartRegistration(loaded->account_signing_public_key_b64, nickname, "ml-dsa-65",
                                              loaded->kem_public_key_b64, loaded->peer_id, multiaddrs, publish);
  if (!start) {
    return start.error();
  }

  const int64_t timestamp = util::NowUnixMs();
  const auto sign_bytes = BuildRegistrationSignBytes(start->challenge, loaded->account_signing_public_key_b64,
                                                     loaded->kem_public_key_b64, start->signature_alg, timestamp);
  if (sign_bytes.empty()) {
    return Error("Failed to build registration sign bytes");
  }

  auto signature = identity.SignBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }

  return registration.FinishRegistration(start->challenge, loaded->account_signing_public_key_b64, nickname,
                                         *signature, timestamp, start->signature_alg, loaded->kem_public_key_b64,
                                         loaded->peer_id, multiaddrs, loaded->initiation_floor, publish);
}

Roe<LocalIdentity> FinishAndPersistRegistration(IRegistrationClient& registration, IdentityStore& identity,
                                                const std::string& nickname,
                                                const std::vector<std::string>& multiaddrs,
                                                const RegistrationPublishOpts& publish) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }

  auto result = FinishRegistrationWithIdentity(registration, identity, nickname, multiaddrs, publish);
  if (!result) {
    return result.error();
  }

  LocalIdentity updated = *loaded;
  ApplyRegistrationResult(updated, *result);
  if (auto saved = identity.Update(updated); !saved) {
    return saved.error();
  }
  return updated;
}

Roe<bool> MaybeAutoRenewRegistration(IRegistrationClient& registration, IdentityStore& identity,
                                     bool auto_renew_enabled) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  if (!ShouldRenewRegistration(*loaded)) {
    return false;
  }
  if (!auto_renew_enabled) {
    return false;
  }

  auto applied = FinishAndPersistRegistration(registration, identity, loaded->nickname);
  if (!applied) {
    return applied.error();
  }
  return true;
}

Roe<RegistrationResult> UpdateRegisteredNickname(IRegistrationClient& registration, IdentityStore& identity,
                                                 const std::string& nickname) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  if (loaded->relay_user_id.empty()) {
    return Error("relay_user_id not set");
  }

  const int64_t timestamp = util::NowUnixMs();
  const auto sign_bytes = BuildProfileUpdateSignBytes(loaded->relay_user_id, nickname, timestamp);
  if (sign_bytes.empty()) {
    return Error("Failed to build profile sign bytes");
  }

  auto signature = identity.SignBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }

  return registration.UpdateNickname(nickname, *signature, timestamp, loaded->relay_user_id);
}

} // namespace pbr
