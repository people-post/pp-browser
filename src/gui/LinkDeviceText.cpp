#include "gui/LinkDeviceText.h"

#include "foundation/crypto/LinkHandshake.h"
#include "foundation/error/AppError.h"
#include "foundation/i18n/LocalizationService.h"

namespace pbr {

std::string LinkErrorText(const Error& error, const bool on_new_device) {
  if (AppError::IsNewerFormat(error)) {
    return Tr(on_new_device ? "pin.link_err_newer" : "settings.security.link_device.err_newer");
  }
  if (IsLinkError(error, LinkError::Unreadable)) {
    return Tr(on_new_device ? "pin.link_err_unreadable" : "settings.security.link_device.err_unreadable");
  }
  if (on_new_device && IsLinkError(error, LinkError::NotForThisDevice)) {
    return Tr("pin.link_err_not_for_this_device");
  }
  if (on_new_device && IsLinkError(error, LinkError::Expired)) {
    return Tr("pin.link_err_expired");
  }
  return {};
}

} // namespace pbr
