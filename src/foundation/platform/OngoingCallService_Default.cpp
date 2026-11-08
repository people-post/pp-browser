#include "foundation/platform/OngoingCallService.h"

#include <utility>

namespace pbr::OngoingCallService {

void SetOngoingCall(const bool on, const std::string& title) {
  (void)on;
  (void)title;
}

void SetHangUpHandler(std::function<void()> fn) {
  (void)std::move(fn);
}

} // namespace pbr::OngoingCallService
