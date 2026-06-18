#include "agent/tools/WebSearchTool.h"

#include "log/Logger.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <regex>
#include <sstream>

namespace pbr {

namespace {

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* out) {
  const size_t total = size * nmemb;
  out->append(static_cast<char*>(contents), total);
  return total;
}

std::string UrlEncode(const std::string& value) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    return value;
  }
  char* encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
  std::string result = encoded ? encoded : value;
  if (encoded) {
    curl_free(encoded);
  }
  curl_easy_cleanup(curl);
  return result;
}

std::vector<std::string> DefaultHeaders() {
  return {"User-Agent: pp-browser/0.1 (web search tool)"};
}

std::string Trim(const std::string& text) {
  const auto start = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
  const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  if (start >= end) {
    return {};
  }
  return std::string(start, end);
}

std::string Lower(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

bool WantsNewsHeadlines(const std::string& user_message) {
  const std::string text = Lower(user_message);
  if (text.empty()) {
    return false;
  }
  if (Contains(text, "headline")) {
    return true;
  }
  if (Contains(text, "news") &&
      (Contains(text, "today") || Contains(text, "latest") || Contains(text, "breaking") ||
       Contains(text, "current") || Contains(text, "top stories"))) {
    return true;
  }
  return Contains(text, "news about") || Contains(text, "latest news");
}

std::string StripLeadingPhrases(std::string text) {
  static const char* prefixes[] = {
      "please ", "can you ", "could you ", "would you ", "show me ", "tell me ", "give me ",
      "what are ", "what is ", "what's ", "whats ", "find me ", "search for ", "look up ",
      "get me ", "i want ", "i need ", "some ", "the ", "a ",
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

std::string NormalizeWebSearchQuery(const std::string& user_message) {
  const std::string trimmed = Trim(user_message);
  if (trimmed.empty()) {
    return trimmed;
  }

  if (WantsNewsHeadlines(trimmed)) {
    const std::string text_lower = Lower(trimmed);
    const std::string key = "about ";
    const size_t pos = text_lower.find(key);
    if (pos != std::string::npos) {
      std::string topic = Trim(text_lower.substr(pos + key.size()));
      if (!topic.empty()) {
        return topic + " news";
      }
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

Roe<std::string> HttpGet(const std::string& url, const std::vector<std::string>& extra_headers = {}) {
  std::string response;
  CURL* curl = curl_easy_init();
  if (!curl) {
    return Error("curl init failed");
  }

  struct curl_slist* headers = nullptr;
  for (const std::string& header : DefaultHeaders()) {
    headers = curl_slist_append(headers, header.c_str());
  }
  for (const std::string& header : extra_headers) {
    headers = curl_slist_append(headers, header.c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  const CURLcode code = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (code != CURLE_OK) {
    return Error(std::string("curl failed: ") + curl_easy_strerror(code));
  }
  return response;
}

Roe<std::string> HttpPost(const std::string& url, const std::string& body,
                          const std::vector<std::string>& extra_headers = {}) {
  std::string response;
  CURL* curl = curl_easy_init();
  if (!curl) {
    return Error("curl init failed");
  }

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
  for (const std::string& header : DefaultHeaders()) {
    headers = curl_slist_append(headers, header.c_str());
  }
  for (const std::string& header : extra_headers) {
    headers = curl_slist_append(headers, header.c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  const CURLcode code = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (code != CURLE_OK) {
    return Error(std::string("curl failed: ") + curl_easy_strerror(code));
  }
  return response;
}

std::string StripHtmlTags(std::string text) {
  static const std::regex tag_re("<[^>]*>");
  text = std::regex_replace(text, tag_re, "");
  static const std::regex ws_re("\\s+");
  return std::regex_replace(text, ws_re, " ");
}

nlohmann::json ParseDuckDuckGoInstantAnswer(const nlohmann::json& json) {
  nlohmann::json results = nlohmann::json::array();
  const auto push_result = [&results](const std::string& title, const std::string& snippet,
                                      const std::string& url_value) {
    if (title.empty() && snippet.empty()) {
      return;
    }
    results.push_back({{"title", title}, {"snippet", snippet}, {"url", url_value}});
  };

  if (json.contains("AbstractText") && json["AbstractText"].is_string()) {
    const std::string abstract_text = json["AbstractText"].get<std::string>();
    if (!abstract_text.empty()) {
      push_result(json.value("Heading", ""), abstract_text, json.value("AbstractURL", ""));
    }
  }

  if (json.contains("RelatedTopics") && json["RelatedTopics"].is_array()) {
    for (const auto& topic : json["RelatedTopics"]) {
      if (topic.contains("Text") && topic["Text"].is_string()) {
        push_result(topic["Text"].get<std::string>(), "", topic.value("FirstURL", ""));
      } else if (topic.contains("Topics") && topic["Topics"].is_array()) {
        for (const auto& nested : topic["Topics"]) {
          push_result(nested.value("Text", ""), "", nested.value("FirstURL", ""));
        }
      }
    }
  }

  return results;
}

nlohmann::json ParseDuckDuckGoLiteHtml(const std::string& html) {
  nlohmann::json results = nlohmann::json::array();

  static const std::regex link_re(
      R"(<a[^>]*href=['"]([^'"]*)['"][^>]*class=['"]result-link['"][^>]*>([^<]*)</a>)");
  static const std::regex snippet_re(R"(class=['"]result-snippet['"][^>]*>\s*([\s\S]*?)\s*</td>)");

  auto link_it = std::sregex_iterator(html.begin(), html.end(), link_re);
  const auto link_end = std::sregex_iterator();
  auto snippet_it = std::sregex_iterator(html.begin(), html.end(), snippet_re);
  const auto snippet_end = std::sregex_iterator();

  for (; link_it != link_end && results.size() < 8; ++link_it) {
    const std::string url = (*link_it)[1].str();
    const std::string title = StripHtmlTags((*link_it)[2].str());
    std::string snippet;
    if (snippet_it != snippet_end) {
      snippet = StripHtmlTags((*snippet_it)[1].str());
      ++snippet_it;
    }
    results.push_back({{"title", title}, {"snippet", snippet}, {"url", url}});
  }

  return results;
}

nlohmann::json ParseDuckDuckGoHtmlResults(const std::string& html) {
  nlohmann::json results = nlohmann::json::array();

  static const std::regex link_re(
      R"(<a[^>]*href=['"]([^'"]*)['"][^>]*class=['"]result__a['"][^>]*>([^<]*)</a>)");
  static const std::regex snippet_re(R"(class=['"]result__snippet['"][^>]*>([\s\S]*?)</a>)");

  auto link_it = std::sregex_iterator(html.begin(), html.end(), link_re);
  const auto link_end = std::sregex_iterator();
  auto snippet_it = std::sregex_iterator(html.begin(), html.end(), snippet_re);
  const auto snippet_end = std::sregex_iterator();

  for (; link_it != link_end && results.size() < 8; ++link_it) {
    const std::string url = (*link_it)[1].str();
    const std::string title = StripHtmlTags((*link_it)[2].str());
    std::string snippet;
    if (snippet_it != snippet_end) {
      snippet = StripHtmlTags((*snippet_it)[1].str());
      ++snippet_it;
    }
    results.push_back({{"title", title}, {"snippet", snippet}, {"url", url}});
  }

  return results;
}

std::string DecodeBasicHtmlEntities(std::string text) {
  struct Entity {
    const char* encoded;
    const char* decoded;
  };
  static const Entity entities[] = {
      {"&amp;", "&"},  {"&lt;", "<"},   {"&gt;", ">"},   {"&quot;", "\""}, {"&#39;", "'"},
      {"&apos;", "'"}, {"&#x27;", "'"}, {"&#x2F;", "/"}, {"&nbsp;", " "},
  };
  for (const Entity& entity : entities) {
    size_t pos = 0;
    const size_t encoded_len = std::strlen(entity.encoded);
    while ((pos = text.find(entity.encoded, pos)) != std::string::npos) {
      text.replace(pos, encoded_len, entity.decoded);
      pos += std::strlen(entity.decoded);
    }
  }
  return text;
}

std::string ExtractXmlTag(const std::string& xml, const std::string& tag) {
  const std::string open = "<" + tag + ">";
  const std::string close = "</" + tag + ">";
  const size_t start = xml.find(open);
  if (start == std::string::npos) {
    return {};
  }
  const size_t content_start = start + open.size();
  const size_t end = xml.find(close, content_start);
  if (end == std::string::npos) {
    return {};
  }
  return DecodeBasicHtmlEntities(xml.substr(content_start, end - content_start));
}

nlohmann::json ParseGoogleNewsRssXml(const std::string& xml) {
  nlohmann::json results = nlohmann::json::array();

  static const std::regex item_re(R"(<item>([\s\S]*?)</item>)");
  auto begin = std::sregex_iterator(xml.begin(), xml.end(), item_re);
  const auto end = std::sregex_iterator();

  for (auto it = begin; it != end && results.size() < 8; ++it) {
    const std::string item_xml = (*it)[1].str();
    const std::string title = ExtractXmlTag(item_xml, "title");
    const std::string link = ExtractXmlTag(item_xml, "link");
    const std::string description = ExtractXmlTag(item_xml, "description");
    if (title.empty() || title == "Google News") {
      continue;
    }

    std::string snippet = description.empty() ? title : description;
    results.push_back({{"title", title}, {"snippet", snippet}, {"url", link}});
  }

  return results;
}

Roe<std::string> SearchGoogleNewsRss(const std::string& query) {
  const std::string url = query.empty()
                              ? "https://news.google.com/rss?hl=en-US&gl=US&ceid=US:en"
                              : "https://news.google.com/rss/search?q=" + UrlEncode(query) + "&hl=en-US&gl=US&ceid=US:en";
  logging::getLogger("WebSearchTool").info << "web_search (google news rss) query: " << query;

  auto body = HttpGet(url);
  if (!body) {
    logging::getLogger("WebSearchTool").warning << "Google News RSS failed: " << body.error().message;
    return body.error();
  }

  nlohmann::json results = ParseGoogleNewsRssXml(*body);
  if (results.empty()) {
    return Error("Google News RSS returned no items");
  }

  logging::getLogger("WebSearchTool").info << "web_search (google news rss) returned " << results.size()
                                           << " result(s)";
  return nlohmann::json{{"results", results}, {"source", "google_news_rss"}}.dump();
}

Roe<std::string> SearchDuckDuckGo(const std::string& query) {
  logging::getLogger("WebSearchTool").info << "web_search query: " << query;

  const std::string url =
      "https://api.duckduckgo.com/?q=" + UrlEncode(query) + "&format=json&no_html=1&skip_disambig=1";
  auto body = HttpGet(url);
  if (!body) {
    logging::getLogger("WebSearchTool").error << "DuckDuckGo API failed: " << body.error().message;
    return body.error();
  }

  auto json = nlohmann::json::parse(*body, nullptr, false);
  if (json.is_discarded()) {
    return Error("Failed to parse DuckDuckGo response");
  }

  nlohmann::json results = ParseDuckDuckGoInstantAnswer(json);

  if (results.empty()) {
    logging::getLogger("WebSearchTool").debug << "Instant answer empty; trying DuckDuckGo lite POST";
    const std::string post_body = "q=" + UrlEncode(query);
    auto html = HttpPost("https://lite.duckduckgo.com/lite/", post_body);
    if (html) {
      results = ParseDuckDuckGoLiteHtml(*html);
    }
  }

  if (results.empty()) {
    logging::getLogger("WebSearchTool").debug << "Lite empty; trying DuckDuckGo HTML POST";
    const std::string post_body = "q=" + UrlEncode(query);
    auto html = HttpPost("https://html.duckduckgo.com/html/", post_body);
    if (html) {
      results = ParseDuckDuckGoHtmlResults(*html);
    }
  }

  if (results.empty()) {
    logging::getLogger("WebSearchTool").warning << "No search results for query: " << query;
    return nlohmann::json{{"results", nlohmann::json::array()},
                          {"message", "No results found. Try rephrasing the query."}}
        .dump();
  }

  logging::getLogger("WebSearchTool").info << "web_search returned " << results.size() << " result(s)";
  return nlohmann::json{{"results", results}}.dump();
}

Roe<std::string> SearchTavily(const SearchConfig& config, const std::string& query) {
  logging::getLogger("WebSearchTool").info << "web_search (tavily) query: " << query;

  if (config.api_key.empty()) {
    return Error("Tavily API key not configured");
  }

  nlohmann::json body = {{"api_key", config.api_key}, {"query", query}, {"max_results", 5}};
  std::string response;
  CURL* curl = curl_easy_init();
  if (!curl) {
    return Error("curl init failed");
  }

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  const std::string payload = body.dump();

  curl_easy_setopt(curl, CURLOPT_URL, "https://api.tavily.com/search");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

  const CURLcode code = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (code != CURLE_OK) {
    return Error(std::string("curl failed: ") + curl_easy_strerror(code));
  }

  auto json = nlohmann::json::parse(response, nullptr, false);
  if (json.is_discarded()) {
    return Error("Failed to parse Tavily response");
  }

  nlohmann::json results = nlohmann::json::array();
  for (const auto& item : json.value("results", nlohmann::json::array())) {
    results.push_back({{"title", item.value("title", "")},
                       {"snippet", item.value("content", "")},
                       {"url", item.value("url", "")}});
  }

  logging::getLogger("WebSearchTool").info << "web_search (tavily) returned " << results.size() << " result(s)";
  return nlohmann::json{{"results", results}}.dump();
}

} // namespace

ToolDescriptor WebSearchTool::Make(const SearchConfig& config) {
  ToolDescriptor tool;
  tool.definition = ToolDefinition{
      .name = "web_search",
      .description = "Search the web for up-to-date information. Returns numbered result lines "
                     "(title, snippet, url). Use for news, current events, prices, weather, and recent facts. "
                     "Call again with a refined query if the first results are incomplete or off-topic.",
      .parameters = {{"type", "object"},
                     {"properties", {{"query", {{"type", "string"}, {"description", "Concise search query"}}}}},
                     {"required", nlohmann::json::array({"query"})}},
  };
  tool.execute = [config](const nlohmann::json& arguments) -> Roe<std::string> {
    return Search(config, arguments);
  };
  return tool;
}

Roe<std::string> WebSearchTool::Search(const SearchConfig& config, const nlohmann::json& arguments) {
  const std::string query = arguments.value("query", "");
  if (query.empty()) {
    return Error("web_search requires a query");
  }

  const std::string normalized_query = NormalizeWebSearchQuery(query);

  if (config.provider == "tavily") {
    return SearchTavily(config, normalized_query);
  }

  if (WantsNewsHeadlines(query) || WantsNewsHeadlines(normalized_query)) {
    if (auto rss = SearchGoogleNewsRss(normalized_query)) {
      return *rss;
    }
  }

  return SearchDuckDuckGo(normalized_query);
}

nlohmann::json WebSearchTool::ParseDuckDuckGoInstantAnswerJson(const std::string& json_text) {
  auto json = nlohmann::json::parse(json_text, nullptr, false);
  if (json.is_discarded()) {
    return nlohmann::json::array();
  }
  return ParseDuckDuckGoInstantAnswer(json);
}

nlohmann::json WebSearchTool::ParseDuckDuckGoLiteHtmlResults(const std::string& html) {
  return ParseDuckDuckGoLiteHtml(html);
}

nlohmann::json WebSearchTool::ParseDuckDuckGoHtmlPageResults(const std::string& html) {
  return ParseDuckDuckGoHtmlResults(html);
}

nlohmann::json WebSearchTool::ParseGoogleNewsRssItems(const std::string& xml) {
  return ParseGoogleNewsRssXml(xml);
}

} // namespace pbr
