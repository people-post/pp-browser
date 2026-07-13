#include "base/ai/LlmClient.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace pbr {

namespace {

std::string JsonStringOrDefault(const nlohmann::json& json, const char* key,
                                const std::string& default_value = {}) {
  if (!json.contains(key)) {
    return default_value;
  }
  const auto& value = json[key];
  if (value.is_string()) {
    return value.get<std::string>();
  }
  return default_value;
}

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* out) {
  const size_t total = size * nmemb;
  out->append(static_cast<char*>(contents), total);
  return total;
}

nlohmann::json MessageToJson(const ChatMessage& message) {
  nlohmann::json out = {{"role", message.role}, {"content", message.content}};
  if (message.tool_call_id) {
    out["tool_call_id"] = *message.tool_call_id;
  }
  if (message.tool_calls) {
    out["tool_calls"] = *message.tool_calls;
  }
  return out;
}

nlohmann::json ToolsToJson(const std::vector<ToolDefinition>& tools) {
  nlohmann::json out = nlohmann::json::array();
  for (const ToolDefinition& tool : tools) {
    out.push_back({{"type", "function"},
                   {"function",
                    {{"name", tool.name},
                     {"description", tool.description},
                     {"parameters", tool.parameters.empty() ? nlohmann::json::object() : tool.parameters}}}});
  }
  return out;
}

bool IsOllamaEndpoint(const std::string& base_url) {
  return base_url.find("11434") != std::string::npos || base_url.find("ollama") != std::string::npos;
}

std::string MapHttpErrorMessage(long http_code, const std::string& response_body) {
  auto json = nlohmann::json::parse(response_body, nullptr, false);
  std::string message;
  std::string code;
  if (!json.is_discarded() && json.contains("error")) {
    const auto& err = json["error"];
    if (err.is_object()) {
      if (err.contains("message") && err["message"].is_string()) {
        message = err["message"].get<std::string>();
      }
      if (err.contains("code") && err["code"].is_string()) {
        code = err["code"].get<std::string>();
      }
    } else if (err.is_string()) {
      message = err.get<std::string>();
    }
  }
  if (http_code == 429) {
    return message.empty()
               ? "Brief assistant is busy; try later or use your own API key."
               : message;
  }
  if (http_code == 401 || http_code == 403 || code == "not_registered" || code == "auth_error") {
    return message.empty()
               ? "Register or rotate your Brief API key in Me → Profile."
               : message;
  }
  if (!message.empty()) {
    return "LLM API error: " + message;
  }
  return "LLM HTTP " + std::to_string(http_code);
}

} // namespace

LlmClient::LlmClient(LlmConfig config) : config_(std::move(config)) {
  redirectLogger("LlmClient");
}

LlmClient::LlmClient(const LlmClient& other) : Module(), config_(other.config_) {
  redirectLogger("LlmClient");
}

Roe<std::string> LlmClient::Complete(const std::string& system_prompt, const std::string& user_prompt) const {
  ChatCompletionRequest request;
  request.messages = {
      {ChatMessage{.role = "system", .content = system_prompt}},
      {ChatMessage{.role = "user", .content = user_prompt}},
  };
  auto result = Complete(request);
  if (!result) {
    return result.error();
  }
  if (!result->content) {
    return Error("LLM response missing content");
  }
  return *result->content;
}

std::vector<ChatMessage> LlmClient::CoalesceLeadingSystemMessages(std::vector<ChatMessage> messages) {
  size_t leading_systems = 0;
  while (leading_systems < messages.size() && messages[leading_systems].role == "system") {
    ++leading_systems;
  }
  if (leading_systems <= 1) {
    return messages;
  }

  std::string merged = messages[0].content;
  for (size_t i = 1; i < leading_systems; ++i) {
    if (merged.empty()) {
      merged = messages[i].content;
    } else if (!messages[i].content.empty()) {
      merged += "\n\n";
      merged += messages[i].content;
    }
  }

  std::vector<ChatMessage> out;
  out.reserve(messages.size() - leading_systems + 1);
  out.push_back(ChatMessage{.role = "system", .content = std::move(merged)});
  out.insert(out.end(), messages.begin() + static_cast<std::ptrdiff_t>(leading_systems), messages.end());
  return out;
}

Roe<ChatCompletionResponse> LlmClient::Complete(const ChatCompletionRequest& request) const {
  if (config_.require_api_key && config_.api_key.empty()) {
    return Error("LLM API key not configured");
  }

  const std::vector<ChatMessage> wire_messages = CoalesceLeadingSystemMessages(request.messages);
  nlohmann::json messages = nlohmann::json::array();
  for (const ChatMessage& message : wire_messages) {
    messages.push_back(MessageToJson(message));
  }

  nlohmann::json body = {{"model", config_.model}, {"messages", messages}};
  if (config_.num_predict > 0) {
    body["max_tokens"] = config_.num_predict;
    if (IsOllamaEndpoint(config_.base_url)) {
      body["options"] = {{"num_predict", config_.num_predict}};
    }
  }
  if (!request.tools.empty()) {
    body["tools"] = ToolsToJson(request.tools);
    body["tool_choice"] = "auto";
  }

  const std::string payload = body.dump();
  std::string response;

  CURL* curl = curl_easy_init();
  if (!curl) {
    return Error("curl init failed");
  }

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  std::string bearer_header;
  if (!config_.api_key.empty()) {
    bearer_header = "Authorization: Bearer " + config_.api_key;
    headers = curl_slist_append(headers, bearer_header.c_str());
  }

  const std::string url = config_.base_url + "/chat/completions";
  log().debug << "POST " << url << " model=" << config_.model;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

  const CURLcode code = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (code != CURLE_OK) {
    log().error << "curl failed: " << curl_easy_strerror(code);
    return Error(std::string("curl failed: ") + curl_easy_strerror(code));
  }

  if (http_code >= 400) {
    return Error(MapHttpErrorMessage(http_code, response));
  }

  log().debug << "LLM response received (" << response.size() << " bytes)";
  auto parsed = ParseChatCompletionResponse(response);
  if (parsed && !parsed->finish_reason.empty()) {
    log().debug << "LLM finish_reason=" << parsed->finish_reason;
  }
  return parsed;
}

Roe<ChatCompletionResponse> LlmClient::ParseChatCompletionResponse(const std::string& response) {
  auto json = nlohmann::json::parse(response, nullptr, false);
  if (json.is_discarded()) {
    return Error("Failed to parse LLM response JSON");
  }

  if (json.contains("error")) {
    const auto& err = json["error"];
    const std::string message = err.contains("message") ? err["message"].get<std::string>() : err.dump();
    return Error("LLM API error: " + message);
  }

  if (!json.contains("choices") || !json["choices"].is_array() || json["choices"].empty()) {
    return Error("LLM response missing choices");
  }

  const auto& choice = json["choices"][0];
  const auto& message = choice.value("message", nlohmann::json::object());

  ChatCompletionResponse out;
  out.finish_reason = choice.value("finish_reason", "stop");

  if (message.contains("content") && !message["content"].is_null()) {
    if (message["content"].is_string()) {
      out.content = message["content"].get<std::string>();
    } else {
      out.content = message["content"].dump();
    }
  }

  if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
    for (const auto& call : message["tool_calls"]) {
      ToolCall tool_call;
      tool_call.id = JsonStringOrDefault(call, "id");
      if (call.contains("function") && call["function"].is_object()) {
        const auto& fn = call["function"];
        tool_call.name = JsonStringOrDefault(fn, "name");
        const nlohmann::json args_field = fn.value("arguments", nlohmann::json("{}"));
        if (args_field.is_string()) {
          tool_call.arguments = nlohmann::json::parse(args_field.get<std::string>(), nullptr, false);
          if (tool_call.arguments.is_discarded()) {
            tool_call.arguments = nlohmann::json::object();
          }
        } else {
          tool_call.arguments = args_field;
        }
      }
      if (!tool_call.name.empty()) {
        out.tool_calls.push_back(std::move(tool_call));
      }
    }
  }

  return out;
}

} // namespace pbr
