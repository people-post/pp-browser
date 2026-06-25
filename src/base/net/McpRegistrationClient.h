#pragma once

#include "base/ai/mcp/McpClient.h"
#include "base/net/ServiceClients.h"

namespace pbr {

class McpRegistrationClient : public IRegistrationClient {
public:
  explicit McpRegistrationClient(McpClient* client = nullptr);

  void SetClient(McpClient* client);
  Roe<RegistrationResult> Register(const std::string& public_key_b64, const std::string& nickname,
                                   const std::string& signature, int64_t timestamp) override;
  Roe<RegistrationResult> UpdateNickname(const std::string& new_nickname, const std::string& signature,
                                         int64_t timestamp) override;

private:
  McpClient* client_ = nullptr;
};

} // namespace pbr
