#pragma once

#include "foundation/platform/PlatformShareImage.h"
#include "lib/people/ProfileIdentityView.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pbr {

/** This device's own account, for the phone Add contact sheet's "my account" card, QR code and invite. */
struct ContactsAccountPorts {
  std::function<ProfileIdentityView()> load_identity;
  /** The QR page's "Share" button: the system sheet for a picture (PNG from memory); empty where there is none. */
  std::function<bool(const std::vector<uint8_t>& png, ShareAnchor anchor)> share_image;
  /** The QR page's second button: what saving does here (nothing: the button is hidden) and how. */
  ImageSaveKind save_image_kind = ImageSaveKind::None;
  std::function<void(const std::string& suggested_name, std::vector<uint8_t> png,
                     std::function<void(ImageSaveResult)> done)>
      save_image;
};

} // namespace pbr
