#pragma once

#include "lib/messaging/AttachmentDownloadPolicy.h"
#include "lib/messaging/GroupTypes.h"
#include "mesh/types/AddressDisclosure.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/** Inbound group-invite + attachment download policy projected from ProfilePreferences. */
struct HubPolicyPrefs {
  GroupInvitePolicy group_invite_policy = GroupInvitePolicy::ContactsOnly;
  AttachmentDownloadPolicy attachment_download_policy = AttachmentDownloadPolicy::Smart;
  /** "Download everything" (AlwaysAuto) on Wi-Fi / on the mobile network; neither ticked: Smart. */
  bool attachment_auto_on_wifi = true;
  bool attachment_auto_on_cellular = false;
  mesh::InboundAudience call_invite_policy = mesh::InboundAudience::Everyone;

  bool operator==(const HubPolicyPrefs& other) const {
    return group_invite_policy == other.group_invite_policy && call_invite_policy == other.call_invite_policy &&
           attachment_download_policy == other.attachment_download_policy &&
           attachment_auto_on_wifi == other.attachment_auto_on_wifi &&
           attachment_auto_on_cellular == other.attachment_auto_on_cellular;
  }
  bool operator!=(const HubPolicyPrefs& other) const { return !(*this == other); }
};

} // namespace pbr
