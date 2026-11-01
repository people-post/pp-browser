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
 * retired-item cleanup (lib/people RetiredKeychainBackup, at-rest A011) and later by the recovery seed's easy
 * tier (multi-device M023 §3). The app hands in the OS store (foundation/platform OsSecureBlobStore); tests,
 * one in memory.
 */
class ISecureBlobStore {
public:
  virtual ~ISecureBlobStore() = default;
  virtual bool Supported() const = 0;
  virtual Roe<void> Write(const std::string& name, const std::vector<uint8_t>& bytes) = 0;
  virtual Roe<std::optional<std::vector<uint8_t>>> Read(const std::string& name) = 0;
  virtual Roe<void> Delete(const std::string& name) = 0;
};

/** No store: unsupported, writes nothing, reads nothing. */
inline ISecureBlobStore& NoSecureBlobStore() {
  class NoStore final : public ISecureBlobStore {
  public:
    bool Supported() const override { return false; }
    Roe<void> Write(const std::string&, const std::vector<uint8_t>&) override { return {}; }
    Roe<std::optional<std::vector<uint8_t>>> Read(const std::string&) override {
      return std::optional<std::vector<uint8_t>>{};
    }
    Roe<void> Delete(const std::string&) override { return {}; }
  };
  static NoStore store;
  return store;
}

} // namespace pbr
