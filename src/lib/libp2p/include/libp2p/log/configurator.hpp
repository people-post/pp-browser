/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <soralog/impl/configurator_from_yaml.hpp>

namespace libp2p::log {

  class Configurator : public soralog::ConfiguratorFromYAML {
   public:
    Configurator();

    explicit Configurator(std::string config);
  };

}  // namespace libp2p::log
