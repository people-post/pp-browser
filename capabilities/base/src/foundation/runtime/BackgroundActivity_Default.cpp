#include "foundation/runtime/BackgroundActivity.h"

namespace pbr {

struct BackgroundActivity::State {};

BackgroundActivity::BackgroundActivity(const std::string& /*name*/) {
  ++Held();
}

BackgroundActivity::~BackgroundActivity() {
  --Held();
}

} // namespace pbr
