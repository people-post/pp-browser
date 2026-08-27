#include "base/ai/LlmClient.h"

#include "base/error/AppError.h"
#include "base/net/CurlSsl.h"
#include "common/ValueJson.h"

#include <curl/curl.h>

namespace pbr {

namespace {

std::string JsonStringOrDefault(const Object& json, const char* key, const std::string& default_value = {}) {
  return json.getString(key).value_or(default_value);
}

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* out) {
  const size_t total = size * nmemb;
  out->append(static_cast<char*>(contents), total);
  return total;
}

Object MessageToObject(const ChatMessage& message) {
  Object out;
  out.set("role", message.role);
  out.set("content", message.content);
  if (message.tool_call_id) {
    out.set("tool_call_id", *message.tool_call_id);
  }
  if (message.tool_calls) {
    out.set("tool_calls", *message.tool_calls);
  }
  return out;
}

Value ToolsToValue(const std::vector<ToolDefinition>& tools) {
  std::vector<Value> out;
  out.reserve(tools.size());
  for (const ToolDefinition& tool : tools) {
    Object function;
    function.set("name", tool.name);
    function.set("description", tool.description);
    function.set("parameters", tool.parameters.empty() ? Object{} : tool.parameters);

    Object entry;
    entry.set("type", "function");
    entry.set("function", function);
    out.push_back(ObjectValue(std::move(entry)));
  }
  return ArrayValue(std::move(out));
}

bool IsOllamaEndpoint(const std::string& base_url) {
  return base_url.find("11434") != std::string::npos || base_url.find("ollama") != std::string::npos;
}

Error MapHttpError(long http_code, const std::string& response_body) {
  std::string api_message;
  std::string api_code;
  if (auto json = TryParseObject(response_body)) {
    if (auto err_slot = json->fields().tryGet("error")) {
      const Value& err = err_slot->get();
      if (const Object* err_obj = asObject(err)) {
        if (auto message = err_obj->getString("message")) {
          api_message = *message;
        }
        if (auto code = err_obj->getString("code")) {
          api_code = *code;
        }
      } else if (auto message = asString(err)) {
        api_message = *message;
      }
    }
  }

  const std::string detail = !api_message.empty()
                                 ? ("LLM HTTP " + std::to_string(http_code) + ": " + api_message)
                                 : ("LLM HTTP " + std::to_string(http_code));

  if (http_code == 429) {
    auto err = AppError::Auth(Err::Auth::RateLimited, detail);
    if (!api_message.empty()) {
      err.WithUser(api_message);
    }
    return err;
  }
  if (http_code == 401 || http_code == 403 || api_code == "not_registered" || api_code == "auth_error") {
    auto err = AppError::Auth(Err::Auth::Forbidden, detail);
    if (!api_message.empty()) {
      err.WithUser(api_message);
    }
    return err;
  }
  if (!api_message.empty()) {
    return AppError::Network(Err::Network::HttpError, detail).WithUser("LLM API error: " + api_message);
  }
  return AppError::Network(Err::Network::HttpError, detail);
}

Object AsObjectOrEmpty(const Value& value) {
  if (const Object* object = asObject(value)) {
    return *object;
  }
  return {};
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
    return AppError::Internal("LLM response missing content");
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
    return AppError::Config(Err::Config::MissingKey, "LLM API key not configured");
  }

  const std::vector<ChatMessage> wire_messages = CoalesceLeadingSystemMessages(request.messages);
  std::vector<Value> messages;
  messages.reserve(wire_messages.size());
  for (const ChatMessage& message : wire_messages) {
    messages.push_back(ObjectValue(MessageToObject(message)));
  }

  Object body;
  body.set("model", config_.model);
  body.set("messages", ArrayValue(std::move(messages)));
  if (config_.num_predict > 0) {
    body.set("max_tokens", static_cast<int64_t>(config_.num_predict));
    if (IsOllamaEndpoint(config_.base_url)) {
      Object options;
      options.set("num_predict", static_cast<int64_t>(config_.num_predict));
      body.set("options", options);
    }
  }
  if (!request.tools.empty()) {
    body.set("tools", ToolsToValue(request.tools));
    body.set("tool_choice", "auto");
  }

  const std::string payload = DumpJson(body);
  std::string response;

  CURL* curl = curl_easy_init();
  if (!curl) {
    return AppError::Internal("curl init failed");
  }
  ApplyCurlSslDefaults(curl);

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
    return AppError::Network(Err::Network::Unreachable, std::string("curl failed: ") + curl_easy_strerror(code));
  }

  if (http_code >= 400) {
    return MapHttpError(http_code, response);
  }

  log().debug << "LLM response received (" << response.size() << " bytes)";
  auto parsed = ParseChatCompletionResponse(response);
  if (parsed && !parsed->finish_reason.empty()) {
    log().debug << "LLM finish_reason=" << parsed->finish_reason;
  }
  return parsed;
}

Roe<ChatCompletionResponse> LlmClient::ParseChatCompletionResponse(const std::string& response) {
  auto parsed = ParseValue(response);
  if (!parsed) {
    return AppError::Internal("Failed to parse LLM response JSON");
  }
  const Object* json = asObject(*parsed);
  if (!json) {
    return AppError::Internal("Failed to parse LLM response JSON");
  }

  if (auto err_slot = json->fields().tryGet("error")) {
    const Value& err = err_slot->get();
    std::string message;
    if (const Object* err_obj = asObject(err)) {
      message = err_obj->getString("message").value_or(DumpJson(err));
    } else {
      message = DumpJson(err);
    }
    return AppError::Network(Err::Network::HttpError, "LLM API error: " + message)
        .WithUser("LLM API error: " + message);
  }

  const Array* choices = json->getArray("choices");
  if (!choices || choices->elements.empty()) {
    return AppError::Internal("LLM response missing choices");
  }

  const Object* choice = asObject(choices->elements[0]);
  if (!choice) {
    return AppError::Internal("LLM response missing choices");
  }
  Object message;
  if (const Object* message_obj = choice->getObject("message")) {
    message = *message_obj;
  }

  ChatCompletionResponse out;
  out.finish_reason = JsonStringOrDefault(*choice, "finish_reason", "stop");

  if (auto content_slot = message.fields().tryGet("content")) {
    const Value& content = content_slot->get();
    if (!isNullValue(content)) {
      if (auto text = asString(content)) {
        out.content = *text;
      } else {
        out.content = DumpJson(content);
      }
    }
  }

  if (const Array* tool_calls = message.getArray("tool_calls")) {
    for (const Value& call_value : tool_calls->elements) {
      const Object* call = asObject(call_value);
      if (!call) {
        continue;
      }
      ToolCall tool_call;
      tool_call.id = JsonStringOrDefault(*call, "id");
      if (const Object* fn = call->getObject("function")) {
        tool_call.name = JsonStringOrDefault(*fn, "name");
        Object arguments;
        if (auto args_slot = fn->fields().tryGet("arguments")) {
          const Value& args_field = args_slot->get();
          if (auto args_string = asString(args_field)) {
            if (auto parsed_args = TryParseObject(*args_string)) {
              arguments = std::move(*parsed_args);
            }
          } else if (const Object* args_object = asObject(args_field)) {
            arguments = *args_object;
          }
        }
        tool_call.arguments = std::move(arguments);
      }
      if (!tool_call.name.empty()) {
        out.tool_calls.push_back(std::move(tool_call));
      }
    }
  }

  return out;
}

} // namespace pbr
