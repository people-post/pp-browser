#pragma once

#include "base/people/ContactTypes.h"
#include "base/net/ServiceClients.h"

#include <mutex>
#include <string>
#include <vector>

namespace pbr {

class MockDirectoryClient : public IDirectoryClient {
public:
  Roe<std::vector<DirectoryHit>> SearchPeople(const std::string& query) override;
};

class MockRelayClient : public IRelayClient {
public:
  Roe<void> Send(const RelayEnvelope& envelope) override;
  Roe<RelayPollResult> PollInbox(const std::string& cursor) override;

private:
  std::mutex mutex_;
  std::vector<RelayEnvelope> pending_;
  std::vector<RelayEnvelope> delivered_;
  size_t poll_index_ = 0;
};

class MockRegistrationClient : public IRegistrationClient {
public:
  Roe<RegistrationResult> Register(const std::string& public_key_b64, const std::string& nickname,
                                   const std::string& signature, int64_t timestamp) override;
  Roe<RegistrationResult> UpdateNickname(const std::string& new_nickname, const std::string& signature,
                                         int64_t timestamp) override;
};

class HttpRelayClient : public IRelayClient {
public:
  explicit HttpRelayClient(std::string base_url);
  Roe<void> Send(const RelayEnvelope& envelope) override;
  Roe<RelayPollResult> PollInbox(const std::string& cursor) override;

private:
  std::string base_url_;
};

class HttpDirectoryClient : public IDirectoryClient {
public:
  explicit HttpDirectoryClient(std::string base_url);
  Roe<std::vector<DirectoryHit>> SearchPeople(const std::string& query) override;

private:
  std::string base_url_;
};

class HttpRegistrationClient : public IRegistrationClient {
public:
  explicit HttpRegistrationClient(std::string base_url);
  Roe<RegistrationResult> Register(const std::string& public_key_b64, const std::string& nickname,
                                   const std::string& signature, int64_t timestamp) override;
  Roe<RegistrationResult> UpdateNickname(const std::string& new_nickname, const std::string& signature,
                                         int64_t timestamp) override;

private:
  std::string base_url_;
};

} // namespace pbr
