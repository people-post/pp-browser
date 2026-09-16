#include "domain/people/RegistrationStatus.h"

#include "foundation/platform/os/OsTime.h"
#include "common/Utilities.h"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <optional>

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
    for (size_t i = dot + 1; i < iso.size() && std::isdigit(static_cast<unsigned char>(iso[i])) && digits < 3;
         ++i) {
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

void MarkRegistrationExpired(LocalIdentity& identity) {
  identity.registration_expires_at = kExpiredEpochIso;
}

} // namespace pbr
