#include "base/net/McpRegistrationClient.h"

#include "base/net/McpInfraBridge.h"

namespace pbr {

McpRegistrationClient::McpRegistrationClient(McpClient* client) : client_(client) {}

void McpRegistrationClient::SetClient(McpClient* client) {
  client_ = client;
}

Roe<RegistrationResult> McpRegistrationClient::Register(const std::string& public_key_b64,
                                                        const std::string& nickname, const std::string& signature,
                                                        int64_t timestamp) {
  if (!client_) {
    return Error("MCP client not available");
  }
  auto result = CallMcpToolJson(*client_, "register_user",
                                {{"public_key", public_key_b64},
                                 {"nickname", nickname},
                                 {"timestamp", timestamp},
                                 {"signature", signature}});
  if (!result) {
    return result.error();
  }

  RegistrationResult registration;
  if (result->contains("success") && (*result)["success"].is_boolean()) {
    registration.success = (*result)["success"].get<bool>();
  } else {
    registration.success = true;
  }
  if (result->contains("relay_user_id") && (*result)["relay_user_id"].is_string()) {
    registration.relay_user_id = (*result)["relay_user_id"].get<std::string>();
  }
  if (result->contains("message") && (*result)["message"].is_string()) {
    registration.message = (*result)["message"].get<std::string>();
  }
  return registration;
}

Roe<RegistrationResult> McpRegistrationClient::UpdateNickname(const std::string& new_nickname,
                                                              const std::string& signature, int64_t timestamp) {
  if (!client_) {
    return Error("MCP client not available");
  }
  auto result = CallMcpToolJson(*client_, "update_profile_nickname",
                                {{"nickname", new_nickname}, {"timestamp", timestamp}, {"signature", signature}});
  if (!result) {
    return result.error();
  }

  RegistrationResult registration;
  if (result->contains("success") && (*result)["success"].is_boolean()) {
    registration.success = (*result)["success"].get<bool>();
  } else {
    registration.success = true;
  }
  if (result->contains("message") && (*result)["message"].is_string()) {
    registration.message = (*result)["message"].get<std::string>();
  }
  return registration;
}

} // namespace pbr
