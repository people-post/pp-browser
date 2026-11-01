#pragma once

// Messaging's ports to the relay and to peers: the relay's message API (HttpRelayClient) and peer-direct
// history and chat blobs over Amp (domain/delivery, domain/attachments).

#include "common/Error.h"
#include "common/chat/IDirectMessageClient.h"
#include "common/thread/ChatBlobTypes.h"
#include "common/thread/ChatHistoryTypes.h"
#include "foundation/crypto/IDekConsumer.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

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

/** D060 peer-direct history — `/pp/rpc/history/1.0.0` (rpc kind; own OPEN). */
class IChatHistoryPeerClient {
public:
  virtual ~IChatHistoryPeerClient() = default;
  virtual bool IsPeerReachable(const std::string& peer_identity_value) const = 0;
  virtual Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) = 0;

  /** Prefer on product paths; default wraps sync FetchChatHistory. */
  virtual void FetchChatHistoryAsync(const ChatHistoryRequest& request,
                                     std::function<void(Roe<ChatHistoryResponse>)> on_done) {
    if (on_done) {
      on_done(FetchChatHistory(request));
    }
  }
};

class IChatBlobPeerClient {
public:
  virtual ~IChatBlobPeerClient() = default;
  virtual bool IsPeerReachable(const std::string& peer_identity_value) const = 0;
  virtual Roe<std::vector<uint8_t>> FetchChatBlob(const ChatBlobRequest& request) = 0;
  virtual Roe<void> PushChatBlob(const ChatBlobRequest& request, const std::vector<uint8_t>& ciphertext) = 0;

  virtual void FetchChatBlobAsync(const ChatBlobRequest& request,
                                  std::function<void(Roe<std::vector<uint8_t>>)> on_done) {
    if (on_done) {
      on_done(FetchChatBlob(request));
    }
  }
  virtual void PushChatBlobAsync(const ChatBlobRequest& request, const std::vector<uint8_t>& ciphertext,
                                 std::function<void(Roe<void>)> on_done) {
    if (on_done) {
      on_done(PushChatBlob(request, ciphertext));
    }
  }
};

/** Product blob entry (Amp or libp2p) — DEK + profile wiring for ConversationsHub. */
class IChatBlobPeerService : public IChatBlobPeerClient, public IDekConsumer {
public:
  ~IChatBlobPeerService() override = default;
  virtual void SetProfileDataDir(std::string profile_data_dir) = 0;
  virtual void SetProfileId(std::string profile_id) = 0;
  /** Receiver-side admission for a direct push (sender account, ciphertext size); empty admits all. */
  virtual void SetPushGate(std::function<bool(const std::string& sender, uint64_t ciphertext_bytes)> gate) {
    (void)gate;
  }
};

} // namespace pbr
