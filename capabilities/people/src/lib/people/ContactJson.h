#pragma once

#include "lib/people/ContactTypes.h"
#include "common/directory/DirectoryJson.h"
#include "common/Value.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

std::string TrustLevelToString(TrustLevel level);
TrustLevel TrustLevelFromString(const std::string& value);

pp::common::Object ContactToJson(const Contact& contact);
Contact ContactFromJson(const pp::common::Object& json);

/**
 * Drops a contact's retired per-relay ids (a `relay_user` id from before multi-relay MR001, read as an unknown kind
 * with a `relay:` value): contacts.json 2 → 3, and a locker snapshot written before the cut. The contact keeps its
 * Account ID, which is its relay inbox.
 */
void DropRetiredRelayIds(Contact& contact);

} // namespace pbr
