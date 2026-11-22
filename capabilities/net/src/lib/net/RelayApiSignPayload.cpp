#include "lib/net/RelayApiSignPayload.h"

namespace pbr {

namespace {

std::vector<uint8_t> BuildDeviceSignBytes(const std::string& audience, const RelayApiOp op,
                                          const std::string& account_id, const std::string& platform,
                                          const std::string& device_id, const std::string& last,
                                          const int64_t timestamp, const pp::ExtArea& ext) {
  return BuildRelayApiBytes(audience, op, timestamp, ext, [&](SignedBytesWriter& out) {
    out.Utf8(account_id).Utf8(platform).Utf8(device_id).Utf8(last);
  });
}

} // namespace

std::vector<uint8_t> BuildRelayApiBytes(const std::string& audience, const RelayApiOp op, const int64_t timestamp,
                                        const pp::ExtArea& ext, const RelayCallFields& fields) {
  return BuildRelayCallBytes(kRelayApiDomain, audience, ext, [&](SignedBytesWriter& out) {
    out.U8(static_cast<uint8_t>(op)).I64(timestamp);
    if (fields) {
      fields(out);
    }
  });
}

std::vector<uint8_t> BuildRelayApiDeviceRegisterSignBytes(const std::string& audience, const std::string& account_id,
                                                          const std::string& platform, const std::string& device_id,
                                                          const std::string& push_token, const int64_t timestamp,
                                                          const pp::ExtArea& ext) {
  return BuildDeviceSignBytes(audience, RelayApiOp::DeviceRegister, account_id, platform, device_id, push_token,
                              timestamp, ext);
}

std::vector<uint8_t> BuildRelayApiDeviceUnregisterSignBytes(const std::string& audience, const std::string& account_id,
                                                            const std::string& platform, const std::string& device_id,
                                                            const std::string& binding_id, const int64_t timestamp,
                                                            const pp::ExtArea& ext) {
  return BuildDeviceSignBytes(audience, RelayApiOp::DeviceUnregister, account_id, platform, device_id, binding_id,
                              timestamp, ext);
}

} // namespace pbr
