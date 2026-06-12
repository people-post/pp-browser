#include "agent/SearchIntent.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace pbr {

namespace {

std::string Lower(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

bool ContainsAny(const std::string& text, const std::initializer_list<const char*> needles) {
  for (const char* needle : needles) {
    if (Contains(text, needle)) {
      return true;
    }
  }
  return false;
}

} // namespace

bool ShouldProactiveWebSearch(const std::string& user_message) {
  const std::string text = Lower(user_message);
  if (text.empty()) {
    return false;
  }

  const bool time_sensitive = ContainsAny(text, {"today", "tonight", "this morning", "this week", "this month",
                                                 "right now", "currently", "latest", "recent", "live", "now",
                                                 "yesterday", "breaking", "current"});

  const bool market_or_price = ContainsAny(text, {"stock", "market", "nasdaq", "dow jones", "s&p", "crypto",
                                                   "bitcoin", "ethereum", "exchange rate", "price of", "stock price",
                                                   "earnings", "fed rate", "interest rate"});

  const bool news_or_events = ContainsAny(text, {"news", "headline", "happening", "election", "score", "weather",
                                                 "forecast", "who won", "who is winning"});

  if (time_sensitive && (market_or_price || news_or_events)) {
    return true;
  }

  if (Contains(text, "stock market") || Contains(text, "market summary") || Contains(text, "market today")) {
    return true;
  }

  if (Contains(text, "search the web") || Contains(text, "look up") || Contains(text, "web search")) {
    return true;
  }

  return false;
}

} // namespace pbr
