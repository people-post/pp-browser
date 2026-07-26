#pragma once

#include "common/Error.h"
#include "base/people/ContactTypes.h"
#include "base/messaging/ThreadTypes.h"

#include <functional>
#include <string>
#include <vector>

namespace pbr {

struct RelayPollResult {
  std::vector<RelayEnvelope> messages;
  std::string next_cursor;
};

struct RelayDeleteResult {
  int64_t deleted = 0;
};

class IRelayClient {
public:
  virtual ~IRelayClient() = default;
  virtual Roe<void> Send(const RelayEnvelope& envelope) = 0;
  virtual Roe<RelayPollResult> PollInbox(const std::string& requester_contact_id, const std::string& cursor) = 0;
  virtual Roe<RelayDeleteResult> AckInbox(const std::string& requester_contact_id, const std::string& cursor) = 0;
  virtual Roe<RelayDeleteResult> ClearInbox(const std::string& requester_contact_id,
                                            const std::string& before_created_at) = 0;
  virtual Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) = 0;
};

/** D060 peer-direct history — libp2p `/pp-browser/chat-history/1.0.0`. */
class IChatHistoryPeerClient {
public:
  virtual ~IChatHistoryPeerClient() = default;
  virtual bool IsPeerReachable(const std::string& peer_identity_value) const = 0;
  virtual Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) = 0;
};

class IDirectoryClient {
public:
  virtual ~IDirectoryClient() = default;
  virtual Roe<std::vector<DirectoryHit>> SearchPeople(const std::string& query) = 0;
  virtual Roe<DirectoryHit> LookupRelayUser(const std::string& relay_user_id) = 0;
};

struct RegistrationResult {
  bool success = false;
  std::string relay_user_id;
  std::string message;
  /** Brief LLM API key returned once from register/finish or rotate. */
  std::string llm_api_key;
  /** ISO-8601 registration expiry from register/finish. */
  std::string expires_at;
};

struct RegistrationStartResult {
  std::string challenge;
  std::string signature_alg;
  std::string expires_at;
};

class IRegistrationClient {
public:
  virtual ~IRegistrationClient() = default;
  virtual Roe<RegistrationStartResult> StartRegistration(const std::string& public_key_b64,
                                                         const std::string& nickname,
                                                         const std::string& signature_alg = "ed25519",
                                                         const std::string& kem_public_key_b64 = "") = 0;
  virtual Roe<RegistrationResult> FinishRegistration(const std::string& challenge,
                                                     const std::string& public_key_b64, const std::string& nickname,
                                                     const std::string& signature, int64_t timestamp,
                                                     const std::string& signature_alg = "ed25519",
                                                     const std::string& kem_public_key_b64 = "") = 0;
  virtual Roe<RegistrationResult> UpdateNickname(const std::string& new_nickname, const std::string& signature,
                                                 int64_t timestamp, const std::string& relay_user_id) = 0;
};

} // namespace pbr
