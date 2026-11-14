#include "lib/net/RelayApiSignPayload.h"

#include "lib/net/RelaySignBytes.h"

namespace pbr {

namespace {

constexpr const char kRelayApiDomain[] = "pp-browser:relay-api-v1";
// 3 (multi-relay MR007): the relay the call is for (its PeerId) after the version; 2 named only Account IDs.
constexpr uint8_t kSignVersion = 3;

std::vector<uint8_t> BuildDeviceSignBytes(const std::string& audience, const RelayApiOp op,
                                          const std::string& account_id, const std::string& platform,
                                          const std::string& device_id, const std::string& last,
                                          const int64_t timestamp) {
  std::ostringstream oss;
  if (!AppendRelayApiHeader(oss, audience, op, timestamp)) {
    return {};
  }
  RelaySignAppendWireLenUtf8(oss, account_id);
  RelaySignAppendWireLenUtf8(oss, platform);
  RelaySignAppendWireLenUtf8(oss, device_id);
  RelaySignAppendWireLenUtf8(oss, last);
  return RelaySignOssToBytes(oss);
}

} // namespace

bool AppendRelayApiHeader(std::ostringstream& oss, const std::string& audience, const RelayApiOp op,
                          const int64_t timestamp) {
  if (!RelaySignAppendCommandHeader(oss, kRelayApiDomain, kSignVersion, audience)) {
    return false;
  }
  RelaySignAppendU8(oss, static_cast<uint8_t>(op));
  RelaySignAppendI64(oss, timestamp);
  return true;
}

std::vector<uint8_t> BuildRelayApiDeviceRegisterSignBytes(const std::string& audience, const std::string& account_id,
                                                          const std::string& platform, const std::string& device_id,
                                                          const std::string& push_token, const int64_t timestamp) {
  return BuildDeviceSignBytes(audience, RelayApiOp::DeviceRegister, account_id, platform, device_id, push_token,
                              timestamp);
}

std::vector<uint8_t> BuildRelayApiDeviceUnregisterSignBytes(const std::string& audience, const std::string& account_id,
                                                            const std::string& platform, const std::string& device_id,
                                                            const std::string& binding_id, const int64_t timestamp) {
  return BuildDeviceSignBytes(audience, RelayApiOp::DeviceUnregister, account_id, platform, device_id, binding_id,
                              timestamp);
}

} // namespace pbr
