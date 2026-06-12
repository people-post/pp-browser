#include "agent/tools/WebSearchTool.h"

#include "agent/LlmClient.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

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

Roe<std::string> HttpGet(const std::string& url, const std::vector<std::string>& extra_headers = {}) {
  std::string response;
  CURL* curl = curl_easy_init();
  if (!curl) {
    return Error("curl init failed");
  }

  struct curl_slist* headers = nullptr;
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

Roe<std::string> SearchDuckDuckGo(const std::string& query) {
  const std::string url =
      "https://api.duckduckgo.com/?q=" + UrlEncode(query) + "&format=json&no_html=1&skip_disambig=1";
  auto body = HttpGet(url);
  if (!body) {
    return body.error();
  }

  auto json = nlohmann::json::parse(*body, nullptr, false);
  if (json.is_discarded()) {
    return Error("Failed to parse DuckDuckGo response");
  }

  nlohmann::json results = nlohmann::json::array();
  const auto push_result = [&results](const std::string& title, const std::string& snippet,
                                      const std::string& url_value) {
    if (title.empty() && snippet.empty()) {
      return;
    }
    results.push_back({{"title", title}, {"snippet", snippet}, {"url", url_value}});
  };

  if (json.contains("AbstractText") && json["AbstractText"].is_string()) {
    push_result(json.value("Heading", ""), json["AbstractText"].get<std::string>(),
                json.value("AbstractURL", ""));
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

  return nlohmann::json{{"results", results}}.dump();
}

Roe<std::string> SearchTavily(const SearchConfig& config, const std::string& query) {
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
  return nlohmann::json{{"results", results}}.dump();
}

} // namespace

ToolDescriptor WebSearchTool::Make(const SearchConfig& config) {
  ToolDescriptor tool;
  tool.definition = ToolDefinition{
      .name = "web_search",
      .description = "Search the web for current information. Use when the user asks about recent events or facts "
                     "you cannot answer from memory.",
      .parameters = {{"type", "object"},
                     {"properties", {{"query", {{"type", "string"}, {"description", "Search query"}}}}},
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

  if (config.provider == "tavily") {
    return SearchTavily(config, query);
  }
  return SearchDuckDuckGo(query);
}

} // namespace pbr
