#pragma once

#include "lib/people/ContactTypes.h"
#include "common/thread/ThreadRecordTypes.h"

namespace pbr {

DirectChatTarget DirectChatTargetFromContact(const Contact& contact, ThreadChannel channel);

} // namespace pbr
