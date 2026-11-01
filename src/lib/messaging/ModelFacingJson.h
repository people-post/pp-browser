#pragma once

#include "common/thread/ThreadRecordTypes.h"
#include "lib/people/ContactTypes.h"
#include "common/Value.h"

namespace pbr {

// What the assistant's tools hand to the selected model about people and threads (PB-057). The model
// needs to name and pick; it does not need keys, addresses, endpoints, icons or message previews, and the
// selected model may be a third-party cloud. The user's remark (local.display_name) is private to the
// device and is never included; only the person's own nickname (remote.nickname) names them.
pp::common::Object ContactForModel(const Contact& contact);
pp::common::Object ThreadForModel(const Thread& thread);

} // namespace pbr
