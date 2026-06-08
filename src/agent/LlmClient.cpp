#include "agent/LlmClient.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace ppbrowser {

namespace {

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* out) {
  const size_t total = size * nmemb;
  out->append(static_cast<char*>(contents), total);
  return total;
}

} // namespace

LlmClient::LlmClient(LlmConfig config) : config_(std::move(config)) {}

std::string LlmClient::Complete(const std::string& system_prompt, const std::string& user_prompt) {
  if (config_.api_key.empty()) {
    throw std::runtime_error("LLM API key not configured");
  }

  nlohmann::json body = {
      {"model", config_.model},
      {"messages",
       nlohmann::json::array({{{"role", "system"}, {"content", system_prompt}},
                              {{"role", "user"}, {"content", user_prompt}}})},
  };

  const std::string payload = body.dump();
  std::string response;

  CURL* curl = curl_easy_init();
  if (!curl) {
    throw std::runtime_error("curl init failed");
  }

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  const std::string auth = "Authorization: Bearer " + config_.api_key;
  headers = curl_slist_append(headers, auth.c_str());

  const std::string url = config_.base_url + "/chat/completions";
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

  const CURLcode code = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (code != CURLE_OK) {
    throw std::runtime_error(std::string("curl failed: ") + curl_easy_strerror(code));
  }

  auto json = nlohmann::json::parse(response);
  return json["choices"][0]["message"]["content"].get<std::string>();
}

} // namespace ppbrowser
