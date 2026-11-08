#include "feature/product/UiDemoConfig.h"

namespace pbr {

std::string_view ResolveBootstrapPin(const bool ui_demo, const std::string_view given) {
  return ui_demo ? std::string_view(kUiDemoPin) : given;
}

} // namespace pbr
