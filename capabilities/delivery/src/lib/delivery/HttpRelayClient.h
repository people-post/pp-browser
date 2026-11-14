#pragma once

#include "common/chat/RelayGrant.h"
#include "lib/delivery/RelayClients.h"
#include "lib/net/OrgBackendClients.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The error for a relay poll answered with a non-2xx `status_code`: a 4xx (but 408 / 429) is HttpRejected, the rest
 * HttpError; a 403 whose JSON `code` is "not_registered" (the relay has no active registration for our account) is
 * Auth NotRegistered instead (IsRelayUserNotRegistered).
 */
Error RelayPollHttpError(long status_code, const std::string& body);

/** What a relay send adds for the relay's sender and contact checks (multi-relay MR003 / MR005). Any thread. */
struct RelaySendHooks {
  /** The recipient's grant to us, attached to the send; none: a stranger's send. Never asked for a self-send. */
  std::function<std::optional<RelayGrant>(const std::string& recipient_account_id)> grant_for;
  /**
   * The relay refused the grant we attached (`grant_expired` / `invalid_grant`): drop it and ask the recipient for a
   * new one. The send goes once more without it, as a stranger's.
   */
  std::function<void(const std::string& recipient_account_id)> grant_refused;
  /** Our account's ML-DSA public key (base64), sent when the relay asks for it (`sender_key_required`). */
  std::function<std::string()> sender_public_key;
};

/**
 * The Brief relay's message API over HTTP: send, inbox poll / ack / clear, stream history (signed per call by our
 * account; every party an Account ID, MR001). A send carries the recipient's grant when we hold one, and goes again
 * once with our key when the relay asks for it, or without the grant when the relay refuses it. Its refusals come
 * back by code: `recipient_not_registered` (Err::Network::RecipientNotRegistered), `payment_required`
 * (PaymentRequired, with the floor), `stranger_quota_exceeded` (Err::Auth::StrangerQuota), a rate limit.
 */
class HttpRelayClient : public IRelayClient {
public:
  explicit HttpRelayClient(std::string base_url);
  void SetAuthSigner(RelayAuthSigner signer) { auth_signer_ = std::move(signer); }
  /** Any thread: a send under way keeps the hooks it started with. */
  void SetSendHooks(RelaySendHooks hooks);
  Roe<void> Send(const RelayEnvelope& envelope) override;
  Roe<RelayPollResult> PollInbox(const std::string& requester_account_id, const std::string& cursor) override;
  Roe<RelayDeleteResult> AckInbox(const std::string& requester_account_id, const std::string& cursor) override;
  Roe<RelayDeleteResult> ClearInbox(const std::string& requester_account_id,
                                    const std::string& before_created_at) override;
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) override;

private:
  /** Why the relay refused a send, by its `code`; what to change before sending it again, if anything. */
  enum class SendRetry { None, WithKey, WithoutGrant };

  Roe<std::string> SignRelayApiBytes(const std::vector<uint8_t>& sign_bytes) const;
  /** One POST of `record` (signed here). `retry` says how a refusal may be met by sending again. */
  Roe<void> PostSend(RelayWireSendRecord& record, const RelayEnvelope& envelope, SendRetry& retry);

  RelaySendHooks SendHooks() const;

  std::string base_url_;
  RelayAuthSigner auth_signer_;
  mutable std::mutex hooks_mutex_;
  RelaySendHooks send_hooks_;
};

} // namespace pbr
