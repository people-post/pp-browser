#pragma once

#include "common/thread/ThreadRecordTypes.h"
#include "domain/people/ContactTypes.h"
#include "common/Value.h"

namespace pbr {

// What the assistant's tools hand to the selected model about people and threads (PB-057). The model
// needs to name and pick; it does not need keys, addresses, endpoints, icons or message previews, and the
// selected model may be a third-party cloud. The people-discovery panel reads id, local.display_name and
// remote.nickname, which both keep.
pp::common::Object ContactForModel(const Contact& contact);
pp::common::Object ThreadForModel(const Thread& thread);

} // namespace pbr
