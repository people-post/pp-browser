#pragma once

#include "common/chat/IDirectMessageClient.h"

#include "common/directory/DirectoryTypes.h"
#include "common/Error.h"
#include "common/directory/IDirectoryClient.h"
#include "foundation/crypto/IDekConsumer.h"
#include "common/thread/ChatBlobTypes.h"
#include "common/thread/ChatHistoryTypes.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

using RelayAuthSigner = std::function<Roe<std::string>(const std::vector<uint8_t>&)>;

struct RelayPollResult {
  std::vector<RelayEnvelope> messages;
  std::string next_cursor;
  /** Unix ms relay clock at poll response (absent on older servers). */
  std::optional<int64_t> server_time_ms;
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

/** D060 peer-direct history — `/pp-browser/rpc/1.0.0`. */
inline constexpr const char* kChatHistoryProtocolId = kRpcProtocolId;

class IChatHistoryPeerClient {
public:
  virtual ~IChatHistoryPeerClient() = default;
  virtual bool IsPeerReachable(const std::string& peer_identity_value) const = 0;
  virtual Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) = 0;
};

/** R019 peer-direct attachment blobs — `/pp-browser/blob/1.0.0`. */
inline constexpr const char* kBlobProtocolId = "/pp-browser/blob/1.0.0";
inline constexpr const char* kChatBlobProtocolId = kBlobProtocolId;

class IChatBlobPeerClient {
public:
  virtual ~IChatBlobPeerClient() = default;
  virtual bool IsPeerReachable(const std::string& peer_identity_value) const = 0;
  virtual Roe<std::vector<uint8_t>> FetchChatBlob(const ChatBlobRequest& request) = 0;
  virtual Roe<void> PushChatBlob(const ChatBlobRequest& request, const std::vector<uint8_t>& ciphertext) = 0;
};

/** Product blob entry (Amp or libp2p) — DEK + profile wiring for ConversationsHub. */
class IChatBlobPeerService : public IChatBlobPeerClient, public IDekConsumer {
public:
  ~IChatBlobPeerService() override = default;
  virtual void SetProfileDataDir(std::string profile_data_dir) = 0;
  virtual void SetProfileId(std::string profile_id) = 0;
};

/** Optional register/finish extras (N027 mesh_node publish). */
struct RegistrationPublishOpts {
  /** Empty → omit (server defaults person). Values: person | mesh_node. */
  std::string entity_kind;
  bool has_capabilities = false;
  MeshCapabilitiesAd capabilities;
};

struct RegistrationResult {
  bool success = false;
  std::string relay_user_id;
  std::string message;
  /** Brief LLM API key returned once from register/finish or rotate. */
  std::string llm_api_key;
  /** ISO-8601 registration expiry from register/finish. */
  std::string expires_at;
  /** Echoed initiation floor when server supports it; missing → leave unset (0). */
  int64_t initiation_floor = 0;
  bool initiation_floor_present = false;
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
                                                         const std::string& signature_alg = "ml-dsa-65",
                                                         const std::string& kem_public_key_b64 = "",
                                                         const std::string& peer_id = "",
                                                         const std::vector<std::string>& multiaddrs = {},
                                                         const RegistrationPublishOpts& publish = {}) = 0;
  virtual Roe<RegistrationResult> FinishRegistration(const std::string& challenge,
                                                     const std::string& public_key_b64, const std::string& nickname,
                                                     const std::string& signature, int64_t timestamp,
                                                     const std::string& signature_alg = "ml-dsa-65",
                                                     const std::string& kem_public_key_b64 = "",
                                                     const std::string& peer_id = "",
                                                     const std::vector<std::string>& multiaddrs = {},
                                                     int64_t initiation_floor = 0,
                                                     const RegistrationPublishOpts& publish = {}) = 0;
  virtual Roe<RegistrationResult> UpdateNickname(const std::string& new_nickname, const std::string& signature,
                                                 int64_t timestamp, const std::string& relay_user_id) = 0;
};

} // namespace pbr
