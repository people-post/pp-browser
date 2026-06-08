#include "agent/LlmClient.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace pbr {

namespace {

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* out) {
  const size_t total = size * nmemb;
  out->append(static_cast<char*>(contents), total);
  return total;
}

} // namespace

LlmClient::LlmClient(LlmConfig config) : config_(std::move(config)) {
  redirectLogger("LlmClient");
}

LlmClient::LlmClient(const LlmClient& other) : Module(), config_(other.config_) {
  redirectLogger("LlmClient");
}

Roe<std::string> LlmClient::Complete(const std::string& system_prompt, const std::string& user_prompt) const {
  if (config_.require_api_key && config_.api_key.empty()) {
    return Error("LLM API key not configured");
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
    return Error("curl init failed");
  }

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  if (!config_.api_key.empty()) {
    const std::string auth = "Authorization: Bearer " + config_.api_key;
    headers = curl_slist_append(headers, auth.c_str());
  }

  const std::string url = config_.base_url + "/chat/completions";
  log().debug << "POST " << url << " model=" << config_.model;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);

  const CURLcode code = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (code != CURLE_OK) {
    log().error << "curl failed: " << curl_easy_strerror(code);
    return Error(std::string("curl failed: ") + curl_easy_strerror(code));
  }

  auto json = nlohmann::json::parse(response, nullptr, false);
  if (json.is_discarded()) {
    log().error << "Failed to parse LLM response JSON";
    return Error("Failed to parse LLM response JSON");
  }

  if (json.contains("error")) {
    const auto& err = json["error"];
    const std::string message = err.contains("message") ? err["message"].get<std::string>() : err.dump();
    log().error << "LLM API error: " << message;
    return Error("LLM API error: " + message);
  }

  if (!json.contains("choices") || !json["choices"].is_array() || json["choices"].empty()) {
    return Error("LLM response missing choices");
  }

  log().debug << "LLM response received (" << response.size() << " bytes)";
  return json["choices"][0]["message"]["content"].get<std::string>();
}

} // namespace pbr
