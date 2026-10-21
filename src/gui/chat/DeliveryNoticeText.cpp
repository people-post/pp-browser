#include "gui/chat/DeliveryNoticeText.h"

#include "common/directory/ShortRelayId.h"
#include "foundation/i18n/LocalizationService.h"

namespace pbr {
namespace {

const char* SendKey(const SendFailure failure) {
  switch (failure) {
  case SendFailure::MeshOff:
    return "chat.notice.send.mesh_off";
  case SendFailure::BadAddress:
    return "chat.notice.send.bad_address";
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
  case SendFailure::Unknown:
    break;
  }
  return "chat.notice.send.unknown";
}

} // namespace

std::string ReceiveFailureText(const ReceiveFailure reason, const std::string& peer, const std::string& family) {
  const std::string who = peer.empty() ? Tr("chat.notice.receive.a_peer") : ShortRelayId(peer);
  const std::string why = reason == ReceiveFailure::ApplyFailed && !family.empty()
                              ? Tr("chat.receive_failure.apply_failed_family", {{"family", family}})
                              : Tr(std::string("chat.receive_failure.") + ReceiveFailureWire(reason));
  return Tr("chat.notice.receive_failed", {{"peer", who}, {"reason", why}});
}

std::string DeliveryNoticeText(const DeliveryNotice& notice) {
  switch (notice.kind) {
  case DeliveryNotice::Kind::SendFailed:
    return Tr(SendKey(notice.send));
  case DeliveryNotice::Kind::SentViaRelay:
    return Tr("chat.notice.sent_via_relay");
  case DeliveryNotice::Kind::GroupMembersMissed:
    return Tr("chat.notice.group_members_missed");
  case DeliveryNotice::Kind::ReceiveFailed:
    return ReceiveFailureText(notice.receive, notice.peer, notice.family);
  }
  return {};
}

} // namespace pbr
