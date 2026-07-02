#pragma once

#include "base/people/ContactTypes.h"
#include "base/messaging/ThreadTypes.h"

namespace pbr {

DirectChatTarget DirectChatTargetFromContact(const Contact& contact, ThreadChannel channel);

} // namespace pbr
