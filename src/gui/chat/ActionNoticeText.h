#pragma once

#include "feature/conversations/ActionNotice.h"

#include <string>

namespace pbr {

/** What the user reads after a local action (a toast), in their language. */
std::string ActionNoticeText(const ActionNotice& notice);

} // namespace pbr
