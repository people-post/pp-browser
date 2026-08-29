/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libp2p/event/signal.hpp>

namespace libp2p::event {

  /**
   * Subscription to some event
   */
  class Subscription {
   public:
    explicit Subscription(Connection conn) : connection_{std::move(conn)} {}

    /**
     * Unsubscribe from the event
     */
    void unsubscribe() {
      connection_.disconnect();
    }

   private:
    Connection connection_;
  };

}  // namespace libp2p::event
