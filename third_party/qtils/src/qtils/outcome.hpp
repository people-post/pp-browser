/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

// Standalone Outcome (ned14). Kept free of Boost.Outcome so Boost.Asio can
// coexist without flipping Outcome into the Boost edition.
#include <outcome.hpp>

#include <functional>
#include <system_error>
#include <utility>

#include <qtils/error.hpp>
#include <qtils/macro/common.hpp>

#define QTILS_OUTCOME_UNIQUE_NAME QTILS_UNIQUE_NAME(outcome_res_)

namespace qtils {
  // Match prior Boost.Outcome facade: std::error_code + default_policy.
  template <typename T, typename E = std::error_code>
  using Result = OUTCOME_V2_NAMESPACE::basic_result<
      T,
      E,
      OUTCOME_V2_NAMESPACE::policy::default_policy<T, E, void>>;
}  // namespace qtils

// Prefer our MSVC-safe OUTCOME_TRY over the amalgamation's macros.
#ifdef OUTCOME_TRY
#undef OUTCOME_TRY
#endif
#ifdef BOOST_OUTCOME_TRY
#undef BOOST_OUTCOME_TRY
#endif

#define _OUTCOME_TRY_void(tmp, expr)    \
  auto &&tmp = expr;                    \
  if (tmp.has_error()) {                \
    return std::move(tmp).as_failure(); \
  }
#define _BOOST_OUTCOME_TRY(tmp, out, expr) \
  _OUTCOME_TRY_void(tmp, expr) out = std::move(tmp).value()
#define BOOST_OUTCOME_TRY(out, expr) \
  _BOOST_OUTCOME_TRY(QTILS_OUTCOME_UNIQUE_NAME, out, expr)
#define _OUTCOME_TRY_out(tmp, out, expr) \
  _BOOST_OUTCOME_TRY(tmp, auto &&out, expr)
// MSVC's traditional preprocessor does not rescan computed macro names of the
// form EXPAND(name)(args), and also glues __VA_ARGS__ across nested calls.
// Select the overload and invoke it inside one variadic expand so the call
// arguments are split correctly without /Zc:preprocessor.
#define _OUTCOME_EXPAND(...) __VA_ARGS__
#define _OUTCOME_TRY_SELECT(_1, _2, name, ...) name
#define OUTCOME_TRY(...)                                                 \
  _OUTCOME_EXPAND(_OUTCOME_TRY_SELECT(                                   \
      __VA_ARGS__, _OUTCOME_TRY_out, _OUTCOME_TRY_void)(                 \
      QTILS_OUTCOME_UNIQUE_NAME, __VA_ARGS__))

namespace outcome {
  template <class R>
  using result = qtils::Result<R>;
  using OUTCOME_V2_NAMESPACE::failure;
  using OUTCOME_V2_NAMESPACE::success;
}  // namespace outcome
