#include "foundation/crypto/KemKeyBindingV2.h"

#include "foundation/crypto/SignedBytes.h"

#include "common/Serialize.hpp"

#include <sstream>

namespace pbr {

namespace {

constexpr char kKemBindingDomain[] = "pp-browser:kem-binding";

} // namespace

ByteVector KemBindingV2SignBytes(const std::span<const uint8_t> typed_signing_public_key,
                                 const std::span<const uint8_t> typed_kem_public_key, const pp::ExtArea& ext) {
  return SignedBytesWriter(kKemBindingDomain, kKemBindingV2Version)
      .Typed(typed_signing_public_key)
      .Typed(typed_kem_public_key)
      .Finish(ext);
}

ByteVector KemBindingV2Blob(const std::span<const uint8_t> typed_signature, const pp::ExtArea& ext) {
  ByteVector blob{kKemBindingV2Version};
  ext.AppendTo(blob);
  std::ostringstream out;
  pp::OutputArchive ar(out);
  pp::WireLenBytes signature{ByteVector(typed_signature.begin(), typed_signature.end())};
  ar & signature;
  const std::string packed = out.str();
  blob.insert(blob.end(), packed.begin(), packed.end());
  return blob;
}

} // namespace pbr
