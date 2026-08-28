#pragma once

#include "common/Error.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** Platform push token source (FCM on Android; unsupported on desktop). */
class IPushDeviceRegistrar {
public:
  using TokenChangedFn = std::function<void(const std::string& token)>;

  virtual ~IPushDeviceRegistrar() = default;

  virtual bool IsSupported() const = 0;
  /** Empty error message means unsupported / unavailable. */
  virtual Roe<std::string> GetPushToken() = 0;
  virtual std::string PlatformName() const = 0;
  virtual std::string DeviceId() const = 0;

  static IPushDeviceRegistrar& Instance();
  static void SetInstance(IPushDeviceRegistrar* registrar);

  /** Invoked when a platform push token becomes available or refreshes (may be off UI thread). */
  static void SetTokenChangedHandler(TokenChangedFn handler);
  static void NotifyTokenChanged(const std::string& token);
};

} // namespace pbr
