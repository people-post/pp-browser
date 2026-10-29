#include "gui/chat/MessageTimeLabel.h"

#include "common/CivilTime.h"
#include "foundation/i18n/LocalizationService.h"

#include <cstdio>
#include <ctime>

namespace pbr {
namespace {

bool SameDay(const std::tm& a, const std::tm& b) {
  return a.tm_year == b.tm_year && a.tm_mon == b.tm_mon && a.tm_mday == b.tm_mday;
}

} // namespace

std::string MessageTimeLabel(const int64_t message_ms, const int64_t now_ms) {
  if (message_ms <= 0) {
    return {};
  }
  std::tm msg{};
  std::tm now{};
  if (!pp::civil_time::LocalTime(static_cast<std::time_t>(message_ms / 1000), &msg) ||
      !pp::civil_time::LocalTime(static_cast<std::time_t>(now_ms / 1000), &now)) {
    return {};
  }
  char hhmm[8];
  std::snprintf(hhmm, sizeof hhmm, "%02d:%02d", msg.tm_hour, msg.tm_min);
  const std::string time = hhmm;
  if (SameDay(msg, now)) {
    return time;
  }
  std::tm yesterday = now;
  yesterday.tm_mday -= 1;
  yesterday.tm_isdst = -1;
  std::mktime(&yesterday); // normalizes month / year rollover
  if (SameDay(msg, yesterday)) {
    return Tr("chat.time.yesterday", {{"time", time}});
  }
  const std::map<std::string, std::string> args{{"month", Tr("chat.time.month." + std::to_string(msg.tm_mon + 1))},
                                                {"day", std::to_string(msg.tm_mday)},
                                                {"year", std::to_string(msg.tm_year + 1900)},
                                                {"time", time}};
  return Tr(msg.tm_year == now.tm_year ? "chat.time.this_year" : "chat.time.older", args);
}

} // namespace pbr
