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

bool LocalDay(const int64_t ms, std::tm* out) {
  return ms > 0 && pp::civil_time::LocalTime(static_cast<std::time_t>(ms / 1000), out);
}

} // namespace

std::string MessageClockLabel(const int64_t message_ms) {
  std::tm msg{};
  if (!LocalDay(message_ms, &msg)) {
    return {};
  }
  char hhmm[8];
  std::snprintf(hhmm, sizeof hhmm, "%02d:%02d", msg.tm_hour, msg.tm_min);
  return hhmm;
}

bool SameLocalDay(const int64_t a_ms, const int64_t b_ms) {
  std::tm a{};
  std::tm b{};
  return LocalDay(a_ms, &a) && LocalDay(b_ms, &b) && SameDay(a, b);
}

std::string MessageDateLabel(const int64_t message_ms, const int64_t now_ms) {
  std::tm msg{};
  std::tm now{};
  if (!LocalDay(message_ms, &msg) || !LocalDay(now_ms, &now)) {
    return {};
  }
  if (SameDay(msg, now)) {
    return Tr("chat.date.today");
  }
  std::tm yesterday = now;
  yesterday.tm_mday -= 1;
  yesterday.tm_isdst = -1;
  std::mktime(&yesterday);
  if (SameDay(msg, yesterday)) {
    return Tr("chat.date.yesterday");
  }
  // 2-6 days ago: the weekday name.
  std::tm msg_noon = msg;
  std::tm now_noon = now;
  for (std::tm* t : {&msg_noon, &now_noon}) {
    t->tm_hour = 12;
    t->tm_min = 0;
    t->tm_sec = 0;
    t->tm_isdst = -1;
  }
  const std::time_t msg_s = std::mktime(&msg_noon);
  const long days = static_cast<long>((std::mktime(&now_noon) - msg_s + 43200) / 86400);
  if (days >= 2 && days <= 6) {
    return Tr("home.weekday." + std::to_string(msg_noon.tm_wday));
  }
  const std::map<std::string, std::string> args{{"month", Tr("chat.time.month." + std::to_string(msg.tm_mon + 1))},
                                                {"day", std::to_string(msg.tm_mday)},
                                                {"year", std::to_string(msg.tm_year + 1900)}};
  return Tr(msg.tm_year == now.tm_year ? "chat.date.this_year" : "chat.date.older", args);
}

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
