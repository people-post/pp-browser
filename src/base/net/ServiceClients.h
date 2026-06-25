#pragma once

#include "common/Error.h"
#include "base/people/ContactTypes.h"
#include "base/messaging/ThreadTypes.h"

#include <string>
#include <vector>

namespace pbr {

struct RelayPollResult {
  std::vector<RelayEnvelope> messages;
  std::string next_cursor;
};

class IRelayClient {
public:
  virtual ~IRelayClient() = default;
  virtual Roe<void> Send(const RelayEnvelope& envelope) = 0;
  virtual Roe<RelayPollResult> PollInbox(const std::string& cursor) = 0;
};

class IDirectoryClient {
public:
  virtual ~IDirectoryClient() = default;
  virtual Roe<std::vector<DirectoryHit>> SearchPeople(const std::string& query) = 0;
};

struct RegistrationResult {
  bool success = false;
  std::string relay_user_id;
  std::string message;
};

class IRegistrationClient {
public:
  virtual ~IRegistrationClient() = default;
  virtual Roe<RegistrationResult> Register(const std::string& public_key_b64, const std::string& nickname,
                                           const std::string& signature, int64_t timestamp) = 0;
  virtual Roe<RegistrationResult> UpdateNickname(const std::string& new_nickname, const std::string& signature,
                                                 int64_t timestamp) = 0;
};

} // namespace pbr
