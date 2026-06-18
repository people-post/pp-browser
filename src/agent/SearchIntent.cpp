#include "agent/SearchIntent.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace pbr {

namespace {

std::string Lower(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

std::string Trim(const std::string& text) {
  const auto start = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
  const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  if (start >= end) {
    return {};
  }
  return std::string(start, end);
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

std::string StripLeadingPhrases(std::string text) {
  static const char* prefixes[] = {
      "please ",           "can you ",          "could you ",        "would you ",       "show me ",
      "tell me ",          "give me ",          "what are ",         "what is ",         "what's ",
      "whats ",            "find me ",          "search for ",       "look up ",         "get me ",
      "i want ",           "i need ",           "some ",             "the ",             "a ",
  };

  bool changed = true;
  while (changed) {
    changed = false;
    for (const char* prefix : prefixes) {
      if (text.rfind(prefix, 0) == 0) {
        text.erase(0, std::strlen(prefix));
        changed = true;
        break;
      }
    }
  }

  while (!text.empty() && (text.back() == '?' || text.back() == '.' || text.back() == '!')) {
    text.pop_back();
  }
  return Trim(text);
}

std::string ExtractNewsTopic(const std::string& text_lower) {
  const std::string key = "about ";
  const size_t pos = text_lower.find(key);
  if (pos == std::string::npos) {
    return {};
  }

  std::string topic = Trim(text_lower.substr(pos + key.size()));
  while (!topic.empty() && (topic.back() == '?' || topic.back() == '.' || topic.back() == '!')) {
    topic.pop_back();
  }
  return Trim(topic);
}

} // namespace

bool WantsNewsHeadlines(const std::string& user_message) {
  const std::string text = Lower(user_message);
  if (text.empty()) {
    return false;
  }

  if (Contains(text, "headline")) {
    return true;
  }

  if (Contains(text, "news") &&
      ContainsAny(text, {"today", "latest", "breaking", "current", "headline", "top stories"})) {
    return true;
  }

  return Contains(text, "news about") || Contains(text, "latest news");
}

bool ShouldProactiveWebSearch(const std::string& user_message) {
  const std::string text = Lower(user_message);
  if (text.empty()) {
    return false;
  }

  if (WantsNewsHeadlines(user_message)) {
    return true;
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

  if (Contains(text, "latest news") || Contains(text, "what is the latest")) {
    return true;
  }

  return false;
}

std::string BuildWebSearchQuery(const std::string& user_message) {
  const std::string trimmed = Trim(user_message);
  if (trimmed.empty()) {
    return trimmed;
  }

  if (WantsNewsHeadlines(trimmed)) {
    const std::string topic = ExtractNewsTopic(Lower(trimmed));
    if (!topic.empty()) {
      return topic + " news";
    }
    return "breaking news";
  }

  std::string normalized = StripLeadingPhrases(Lower(trimmed));
  if (normalized.empty()) {
    return trimmed;
  }
  if (normalized.size() > 120) {
    normalized.resize(120);
  }
  return normalized;
}

bool ShouldProactiveContactsList(const std::string& user_message) {
  const std::string text = Lower(user_message);
  if (text.empty()) {
    return false;
  }

  return ContainsAny(text, {"show my contacts", "show contacts", "list contacts", "my contacts", "local contacts",
                            "who are my contacts", "people i know"});
}

bool ShouldProactivePeopleDiscovery(const std::string& user_message) {
  if (ShouldProactiveContactsList(user_message)) {
    return true;
  }

  const std::string text = Lower(user_message);
  if (text.empty()) {
    return false;
  }

  return ContainsAny(text, {"find someone", "find people", "search people", "search for someone", "look for someone",
                            "discover people", "people on the network", "who can i message", "find a person",
                            "find contact", "find contacts", "search the directory", "search directory"});
}

std::string BuildPeopleSearchQuery(const std::string& user_message) {
  if (ShouldProactiveContactsList(user_message)) {
    return StripLeadingPhrases(Lower(Trim(user_message)));
  }

  std::string normalized = StripLeadingPhrases(Lower(Trim(user_message)));
  static const char* phrases[] = {
      "find someone on the network",
      "find someone",
      "find people on the network",
      "find people",
      "search people",
      "search for someone",
      "look for someone",
      "discover people",
      "people on the network",
      "find a person",
      "find contact",
      "find contacts",
      "search the directory",
      "search directory",
  };
  for (const char* phrase : phrases) {
    if (normalized == phrase) {
      return {};
    }
    if (normalized.rfind(phrase, 0) == 0) {
      std::string rest = Trim(normalized.substr(std::strlen(phrase)));
      if (!rest.empty() && (rest.front() == ':' || rest.front() == '-' || rest.front() == ',')) {
        rest = Trim(rest.substr(1));
      }
      return rest;
    }
  }
  return normalized;
}

} // namespace pbr
