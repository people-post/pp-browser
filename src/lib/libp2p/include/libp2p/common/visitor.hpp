/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <type_traits>  // for std::decay
#include <utility>      // for std::forward
#include <variant>      // for std::visit

namespace libp2p {

  template <typename... Lambdas>
  struct lambda_visitor;

  template <typename Lambda, typename... Lambdas>
  struct lambda_visitor<Lambda, Lambdas...>
      : public Lambda, public lambda_visitor<Lambdas...> {
    using Lambda::operator();
    using lambda_visitor<Lambdas...>::operator();

    // NOLINTNEXTLINE(google-explicit-constructor)
    lambda_visitor(Lambda lambda, Lambdas... lambdas)
        : Lambda(lambda), lambda_visitor<Lambdas...>(lambdas...) {}
  };

  template <typename Lambda>
  struct lambda_visitor<Lambda> : public Lambda {
    using Lambda::operator();

    // NOLINTNEXTLINE(google-explicit-constructor)
    lambda_visitor(Lambda lambda) : Lambda(lambda) {}
  };

  /**
   * @brief Convenient in-place compile-time visitor creation, from a set of
   * lambdas
   */
  template <class... Fs>
  constexpr auto make_visitor(Fs &&...fs) {
    using visitor_type = lambda_visitor<std::decay_t<Fs>...>;
    return visitor_type(std::forward<Fs>(fs)...);
  }

  /**
   * @brief In-place visitor for std::variant (and any type std::visit accepts).
   */
  template <typename TVariant, typename... TVisitors>
  constexpr decltype(auto) visit_in_place(TVariant &&variant,
                                          TVisitors &&...visitors) {
    return std::visit(make_visitor(std::forward<TVisitors>(visitors)...),
                      std::forward<TVariant>(variant));
  }

  /// apply Matcher to optional T
  template <typename T, typename Matcher>
  constexpr decltype(auto) match(T &&t, Matcher &&m) {
    return std::forward<T>(t) ? std::forward<Matcher>(m)(*std::forward<T>(t))
                              : std::forward<Matcher>(m)();
  }

  /// construct visitor from Fs and apply it to optional T
  template <typename T, typename... Fs>
  constexpr decltype(auto) match_in_place(T &&t, Fs &&...fs) {
    return match(std::forward<T>(t), make_visitor(std::forward<Fs>(fs)...));
  }
}  // namespace libp2p
