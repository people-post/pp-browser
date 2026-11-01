#pragma once

#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * Small named blobs kept where an app uninstall does not delete them (iOS: the Keychain). Used today by the
 * retired-item cleanup (RetiredKeychainBackup, at-rest A011) and later by the recovery seed's easy tier
 * (multi-device M023 §3). Production: the OS store; tests: in memory.
 */
class ISecureBlobStore {
public:
  virtual ~ISecureBlobStore() = default;
  virtual bool Supported() const = 0;
  virtual Roe<void> Write(const std::string& name, const std::vector<uint8_t>& bytes) = 0;
  virtual Roe<std::optional<std::vector<uint8_t>>> Read(const std::string& name) = 0;
  virtual Roe<void> Delete(const std::string& name) = 0;
};

/** `pbr::os::SecureBlob*` behind the interface; unsupported where the OS has no such store. */
ISecureBlobStore& OsSecureBlobStore();
/** No store: unsupported, writes nothing, reads nothing. */
ISecureBlobStore& NoSecureBlobStore();

} // namespace pbr
