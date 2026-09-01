#pragma once

/** Shared pp-cpp-common / pp-cpp-crypto aliases for AMP (no pp-browser base deps). */
#include "common/Error.h"
#include "crypto/Types.h"

namespace pbr::amp {

using pp::ByteVector;
using pp::Error;
using pp::Roe;

} // namespace pbr::amp

namespace pbr::adp {

using pp::Error;
using pp::Roe;

} // namespace pbr::adp

namespace pbr::test {

using pp::Error;
using pp::Roe;

} // namespace pbr::test
