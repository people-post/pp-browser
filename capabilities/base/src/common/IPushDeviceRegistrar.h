#pragma once

#include "common/Error.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** Platform push token source (FCM on Android, APNs on iOS; unsupported on desktop). */
class IPushDeviceRegistrar {
public:
  using TokenChangedFn = std::function<void(const std::string& token)>;

  virtual ~IPushDeviceRegistrar() = default;

  virtual bool IsSupported() const = 0;
  /** Empty error message means unsupported / unavailable. */
  virtual Roe<std::string> GetPushToken() = 0;
  virtual std::string PlatformName() const = 0;
  virtual std::string DeviceId() const = 0;
  /**
   * A second token for calls only — PushKit (VoIP) on a CallKit iPhone build (P013), registered as
   * `kCallPushPlatform` with device id DeviceId() + `kCallPushDeviceSuffix`. Empty: none.
   */
  virtual std::string CallPushToken() { return {}; }
  static constexpr const char* kCallPushPlatform = "ios_voip";
  static constexpr const char* kCallPushDeviceSuffix = ".voip";

  static IPushDeviceRegistrar& Instance();
  static void SetInstance(IPushDeviceRegistrar* registrar);

  /** Invoked when a platform push token becomes available or refreshes (may be off UI thread). */
  static void SetTokenChangedHandler(TokenChangedFn handler);
  static void NotifyTokenChanged(const std::string& token);
};

} // namespace pbr
