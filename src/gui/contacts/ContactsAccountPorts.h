#pragma once

#include "lib/people/ProfileIdentityView.h"

#include <functional>

namespace pbr {

/** This device's own account, for the phone Add contact sheet's "my account" card, QR code and invite. */
struct ContactsAccountPorts {
  std::function<ProfileIdentityView()> load_identity;
};

} // namespace pbr
