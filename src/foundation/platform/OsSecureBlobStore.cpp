#include "foundation/platform/OsSecureBlobStore.h"

#include "foundation/platform/os/OsSecureBlob.h"

namespace pbr {

namespace {

class OsStore final : public ISecureBlobStore {
public:
  bool Supported() const override { return os::SecureBlobSupported(); }
  Roe<void> Write(const std::string& name, const std::vector<uint8_t>& bytes) override {
    return os::SecureBlobWrite(name, bytes);
  }
  Roe<std::optional<std::vector<uint8_t>>> Read(const std::string& name) override { return os::SecureBlobRead(name); }
  Roe<void> Delete(const std::string& name) override { return os::SecureBlobDelete(name); }
};

} // namespace

ISecureBlobStore& OsSecureBlobStore() {
  static OsStore store;
  return store;
}

} // namespace pbr
