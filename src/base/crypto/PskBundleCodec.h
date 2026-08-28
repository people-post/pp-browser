#pragma once

#include "base/crypto/CryptoTypes.h"
#include "base/crypto/IPskSessionStore.h"

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

struct PskRawImport {
  ByteVector master_psk;
  std::string master_psk_b64;
};

/** JSON codec for `pp-browser-psk-bundle-v1` (E020/D086). */
class PskBundleCodec {
public:
  static Roe<PskRawImport> DecodeRawBase64(std::string_view text);
  static Roe<PskBundleV1> ParseBundleJson(const std::string& json);
  static Roe<std::string> SerializeBundle(const PskBundleV1& bundle);
  static Roe<void> ValidateBundle(const PskBundleV1& bundle);
  static void CapRetiredTail(std::vector<RetiredPskEntry>& retired, uint32_t active_epoch);
  static std::vector<RetiredPskEntry> MergeRetired(const std::vector<RetiredPskEntry>& existing,
                                                   const std::vector<RetiredPskEntry>& incoming);
};

} // namespace pbr
