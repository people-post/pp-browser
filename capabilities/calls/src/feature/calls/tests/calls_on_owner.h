#pragma once

#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "foundation/runtime/ProductOwners.h"

#include <optional>
#include <type_traits>
#include <utility>

namespace pbr {

/**
 * Run `run` on the calls owner and return its result. The calls components run there (the stack
 * dispatches to them); a test that stands in for the stack drives them the same way.
 */
template <typename F>
auto OnCallsOwner(F&& run) -> decltype(run()) {
  using R = decltype(run());
  if constexpr (std::is_void_v<R>) {
    AppRuntime::RunAndWait(kMediaSessionsOwner, [&]() { run(); });
  } else {
    std::optional<R> result;
    AppRuntime::RunAndWait(kMediaSessionsOwner, [&]() { result.emplace(run()); });
    return std::move(*result);
  }
}

} // namespace pbr
