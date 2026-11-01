#pragma once

#include "common/SecureBlobStore.h"

namespace pbr {

/** The OS's uninstall-surviving store (`os::SecureBlob*`: the iOS Keychain; unsupported elsewhere). */
ISecureBlobStore& OsSecureBlobStore();

} // namespace pbr
