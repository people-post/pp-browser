/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <boost/outcome/result.hpp>

#include <qtils/error.hpp>
#include <qtils/macro/common.hpp>

#define QTILS_OUTCOME_UNIQUE_NAME QTILS_UNIQUE_NAME(outcome_res_)

namespace qtils {
  template <typename T, typename E = std::error_code>
  using Result = boost::outcome_v2::
      basic_result<T, E, boost::outcome_v2::policy::default_policy<T, E, void>>;
}  // namespace qtils

// - - - - - - -
// compatibility
// v v v v v v v

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
#define _OUTCOME_OVERLOAD(_1, _2, s, ...) _OUTCOME_TRY_##s
// MSVC's non-conformant preprocessor passes __VA_ARGS__ as a single token
// when used in a nested macro call, causing _OUTCOME_OVERLOAD to receive
// the wrong argument count and select the _void overload instead of _out.
// Wrapping the overload selector in IDENTITY forces a rescan so __VA_ARGS__
// is properly split into individual arguments on MSVC.
#define OUTCOME_TRY(...) \
  IDENTITY(_OUTCOME_OVERLOAD(__VA_ARGS__, out, void))(QTILS_OUTCOME_UNIQUE_NAME, __VA_ARGS__)

namespace outcome {
  template <class R>
  using result = qtils::Result<R>;
  using boost::outcome_v2::failure;
  using boost::outcome_v2::success;
}  // namespace outcome
