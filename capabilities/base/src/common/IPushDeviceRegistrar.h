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
   * A second token for VoIP pushes (calls) — PushKit on a CallKit iPhone build (P016), registered as
   * `kVoipPushPlatform` with device id DeviceId() + `kVoipPushDeviceSuffix`. Empty: none.
   */
  virtual std::string VoipPushToken() { return {}; }
  static constexpr const char* kVoipPushPlatform = "ios_voip";
  static constexpr const char* kVoipPushDeviceSuffix = ".voip";

  static IPushDeviceRegistrar& Instance();
  static void SetInstance(IPushDeviceRegistrar* registrar);

  /** Invoked when a platform push token becomes available or refreshes (may be off UI thread). */
  static void SetTokenChangedHandler(TokenChangedFn handler);
  static void NotifyTokenChanged(const std::string& token);
};

} // namespace pbr
