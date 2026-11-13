#pragma once

#include "lib/delivery/RelayClients.h"
#include "lib/net/OrgBackendClients.h"

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The error for a relay poll answered with a non-2xx `status_code`: a 4xx (but 408 / 429) is HttpRejected, the rest
 * HttpError; a 403 whose JSON `error` is exactly "Relay user not registered" is Auth NotRegistered instead
 * (IsRelayUserNotRegistered).
 */
Error RelayPollHttpError(long status_code, const std::string& body);

/**
 * The error for a relay send answered with a non-2xx `status_code` other than 429: a 404 is HttpNotFound (the recipient
 * is unknown there), carrying "the relay said so" when its JSON `code` is "recipient_not_found"
 * (IsRelayRecipientNotFound); a 403 whose JSON `error` is exactly "Relay user not registered" is Auth NotRegistered
 * (IsRelayUserNotRegistered); a 4xx (but 408 / 429) is HttpRejected, the rest HttpError.
 */
Error RelaySendHttpError(long status_code, const std::string& body);

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
