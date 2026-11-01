#pragma once

#include "lib/delivery/RelayClients.h"
#include "lib/net/OrgBackendClients.h"

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** The Brief relay's message API over HTTP: send, inbox poll / ack / clear, stream history (signed per call). */
class HttpRelayClient : public IRelayClient {
public:
  explicit HttpRelayClient(std::string base_url);
  void SetAuthSigner(RelayAuthSigner signer) { auth_signer_ = std::move(signer); }
  Roe<void> Send(const RelayEnvelope& envelope) override;
  Roe<RelayPollResult> PollInbox(const std::string& requester_contact_id, const std::string& cursor) override;
  Roe<RelayDeleteResult> AckInbox(const std::string& requester_contact_id, const std::string& cursor) override;
  Roe<RelayDeleteResult> ClearInbox(const std::string& requester_contact_id,
                                    const std::string& before_created_at) override;
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) override;

private:
  Roe<std::string> SignRelayApiBytes(const std::vector<uint8_t>& sign_bytes) const;

  std::string base_url_;
  RelayAuthSigner auth_signer_;
};

} // namespace pbr
