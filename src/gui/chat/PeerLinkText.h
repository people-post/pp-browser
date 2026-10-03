#pragma once

#include "feature/conversations/MeshDeliveryOrchestrator.h"

namespace pbr {

/** Locale keys for the peer-link line and banner in a direct chat header; nullptr means show nothing. */
struct PeerLinkText {
  const char* status_key = nullptr;
  const char* banner_key = nullptr;
  bool show_retry = false;
};

/**
 * What a user sees for a direct chat's link state. Built from the link state only — the transport's
 * own failure text (e.g. "amp link manager: dial timeout") stays in the log (dogfood 2026-10-03).
 * A banner appears only when the user's messages cannot go out: while a relay can still carry them,
 * a failed direct link needs nothing from the user.
 */
inline PeerLinkText PeerLinkTextFor(const ThreadPeerLinkView& link) {
  PeerLinkText text;
  switch (link.path_kind) {
  case ThreadPeerPathKind::Direct:
    text.status_key = "chat.link.direct";
    break;
  case ThreadPeerPathKind::ViaHop:
    text.status_key = "chat.link.via_hop";
    break;
  case ThreadPeerPathKind::ViaRelay:
    text.status_key = "chat.link.via_relay";
    break;
  case ThreadPeerPathKind::Connecting:
    text.status_key = "chat.link.connecting";
    break;
  case ThreadPeerPathKind::Degraded:
    if (link.relay_available) {
      text.status_key = "chat.link.via_relay";
    } else {
      text.status_key = "chat.link.retrying";
      text.banner_key = "chat.link.banner.unreachable";
      text.show_retry = link.show_retry;
    }
    break;
  case ThreadPeerPathKind::Failed:
    text.status_key = "chat.link.offline";
    text.banner_key = "chat.link.banner.no_address";
    break;
  case ThreadPeerPathKind::Ready:
    text.status_key = "chat.link.ready";
    break;
  case ThreadPeerPathKind::None:
    break;
  }
  return text;
}

} // namespace pbr
