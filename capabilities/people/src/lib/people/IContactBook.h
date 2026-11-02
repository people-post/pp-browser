#pragma once

#include "common/Error.h"
#include "lib/people/ContactTypes.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * The contacts as a feature that looks people up reads them: all of them, one by identity, and a row
 * written back with what it learned (a peer's mesh id).
 */
class IContactBook {
public:
  virtual ~IContactBook() = default;

  virtual Roe<std::vector<Contact>> List() const = 0;
  /** Exact match on contact.ids[].value; when kind is set, also requires matching kind. */
  virtual Roe<std::optional<Contact>> FindByIdentity(const std::string& identity_value,
                                                     std::optional<mesh::ContactIdKind> kind = std::nullopt) const = 0;
  virtual Roe<Contact> Upsert(const Contact& contact) = 0;
};

} // namespace pbr
