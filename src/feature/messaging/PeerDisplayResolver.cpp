#include "feature/messaging/PeerDisplayResolver.h"

#include "domain/people/ContactJson.h"
#include "foundation/runtime/ProductBranding.h"

namespace pbr {

PeerDisplayResolver::PeerDisplayResolver(ContactsStore& contacts, DirectoryShadowCache& shadows,
                                         GroupRosterStore* group_roster)
    : contacts_(contacts), shadows_(shadows), group_roster_(group_roster) {}

PeerDisplayLabel PeerDisplayResolver::ResolveDirectPeer(const std::string& peer_identity_kind,
                                                        const std::string& peer_identity_value) const {
  PeerDisplayLabel label;
  if (peer_identity_value.empty()) {
    label.title = "Chat";
    return label;
  }

  std::optional<ContactIdKind> kind;
  if (!peer_identity_kind.empty()) {
    kind = ContactIdKindFromString(peer_identity_kind);
  }
  if (auto contact = contacts_.FindByIdentity(peer_identity_value, kind)) {
    if (*contact) {
      const std::string title = FormatContactTitle(**contact);
      if (!title.empty()) {
        label.title = title;
        label.trust = PeerLabelTrust::Contact;
        label.contact_id = (*contact)->id;
        return label;
      }
      label.contact_id = (*contact)->id;
    }
  }

  if (auto shadow = shadows_.Get(peer_identity_value)) {
    const std::string title = FormatDirectoryTitle(*shadow);
    if (!title.empty()) {
      label.title = title;
      label.trust = PeerLabelTrust::DirectoryUnverified;
      return label;
    }
  }

  label.title = ShortRelayId(peer_identity_value);
  label.trust = PeerLabelTrust::RawId;
  return label;
}

PeerDisplayLabel PeerDisplayResolver::ResolveThread(const Thread& thread) const {
  if (thread.kind == ThreadKind::Group) {
    PeerDisplayLabel label;
    label.trust = PeerLabelTrust::Group;

    std::string shared;
    if (group_roster_ && thread.group_id) {
      if (auto meta = group_roster_->LoadMetadata(*thread.group_id)) {
        if (*meta && !(*meta)->title.empty()) {
          shared = (*meta)->title;
        }
      }
    }
    if (shared.empty()) {
      shared = thread.title;
    }
    if (shared.empty()) {
      shared = "Group chat";
    }

    if (!thread.local_title.empty()) {
      label.title = thread.local_title;
      if (shared != thread.local_title) {
        label.shared_title = shared;
      }
      return label;
    }
    label.title = shared;
    return label;
  }

  if (thread.kind == ThreadKind::Ai) {
    PeerDisplayLabel label;
    label.title = thread.title.empty() ? kProductName : thread.title;
    label.trust = PeerLabelTrust::Contact;
    return label;
  }

  PeerDisplayLabel label =
      ResolveDirectPeer(thread.peer_identity_kind, thread.peer_identity_value);
  if (label.title.empty() && !thread.title.empty()) {
    // Prefer short form of stored title when identity is empty.
    label.title = ShortRelayId(thread.title);
  }
  return label;
}

PeerDisplayLabel PeerDisplayResolver::ResolveSender(const std::string& sender_contact_id) const {
  PeerDisplayLabel label;
  if (sender_contact_id == kLocalSelfContactId) {
    label.title = "You";
    label.trust = PeerLabelTrust::Contact;
    return label;
  }
  if (sender_contact_id == kAiAssistantContactId) {
    label.title = "AI";
    label.trust = PeerLabelTrust::Contact;
    return label;
  }
  if (auto contact = contacts_.Get(sender_contact_id)) {
    if (*contact) {
      const std::string title = FormatContactTitle(**contact);
      if (!title.empty()) {
        label.title = title;
        label.trust = PeerLabelTrust::Contact;
        label.contact_id = sender_contact_id;
        return label;
      }
    }
  }
  // Sender may be a raw Account ID (inbound stranger threads).
  return ResolveDirectPeer(ContactIdKindToString(ContactIdKind::Account), sender_contact_id);
}

} // namespace pbr
