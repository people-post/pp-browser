#include "gui/chat/DeliveryNoticeText.h"

#include "common/directory/ShortRelayId.h"
#include "foundation/i18n/LocalizationService.h"

namespace pbr {
namespace {

const char* SendKey(const SendFailure failure) {
  switch (failure) {
    case SendFailure::MeshOff:
      return "chat.notice.send.mesh_off";
    case SendFailure::NoAddress:
      return "chat.notice.send.no_address";
    case SendFailure::BackingOff:
      return "chat.notice.send.backing_off";
    case SendFailure::DialBusy:
      return "chat.notice.send.dial_busy";
    case SendFailure::PeerNoAnswer:
      return "chat.notice.send.no_answer";
    case SendFailure::HandshakeFailed:
      return "chat.notice.send.handshake_failed";
    case SendFailure::DirectUnconfirmed:
      return "chat.notice.send.direct_unconfirmed";
    case SendFailure::RelayNotConfigured:
      return "chat.notice.send.relay_not_configured";
    case SendFailure::RelayBusy:
      return "chat.notice.send.relay_busy";
    case SendFailure::RelayRateLimited:
      return "chat.notice.send.relay_rate_limited";
    case SendFailure::RelayUnreachable:
      return "chat.notice.send.relay_unreachable";
    case SendFailure::RelayRejected:
      return "chat.notice.send.relay_rejected";
    case SendFailure::Unknown:
      break;
  }
  return "chat.notice.send.unknown";
}

} // namespace

std::string ReceiveFailureText(const ReceiveFailure reason, const std::string& peer, const std::string& family) {
  if (reason == ReceiveFailure::GroupInviteNeedsContact) {
    return Tr("chat.receive_failure.group_invite_needs_contact",
              {{"peer", peer.empty() ? Tr("chat.notice.receive.a_peer") : ShortRelayId(peer)}});
  }
  const std::string who = peer.empty() ? Tr("chat.notice.receive.a_peer") : ShortRelayId(peer);
  const std::string why = reason == ReceiveFailure::ApplyFailed && !family.empty()
                              ? Tr("chat.receive_failure.apply_failed_family", {{"family", family}})
                              : Tr(std::string("chat.receive_failure.") + ReceiveFailureWire(reason));
  return Tr("chat.notice.receive_failed", {{"peer", who}, {"reason", why}});
}

std::string SendFailureText(const SendFailure failure) {
  return Tr(SendKey(failure));
}

std::string SendFailureReasonText(const std::optional<SendFailure>& reason, const SendMarkTap::Refusal refusal) {
  switch (refusal) {
    case SendMarkTap::Refusal::None:
      break;
    case SendMarkTap::Refusal::NotText:
      return reason ? SendFailureText(*reason) : Tr("chat.send.not_sent_other");
    case SendMarkTap::Refusal::ThisKind:
      return reason ? Tr("chat.send.reason_then",
                         {{"reason", SendFailureText(*reason)}, {"then", Tr("chat.send.not_resendable")}})
                    : Tr("chat.send.not_resendable");
  }
  return reason ? SendFailureText(*reason) : Tr("chat.send.not_sent");
}

std::string GroupMembersMissedText() {
  return Tr("chat.notice.group_members_missed");
}

std::string DeliveryNoticeText(const DeliveryNotice& notice) {
  switch (notice.kind) {
    case DeliveryNotice::Kind::SendFailed:
      return SendFailureText(notice.send);
    case DeliveryNotice::Kind::GroupMembersMissed:
      return GroupMembersMissedText();
    case DeliveryNotice::Kind::ReceiveFailed:
      return ReceiveFailureText(notice.receive, notice.peer, notice.family);
  }
  return {};
}

} // namespace pbr
