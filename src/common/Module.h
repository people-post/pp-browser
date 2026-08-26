#pragma once

#include "common/Logger.h"

#include <string>

namespace pp {

class Module {
public:
  Module();
  virtual ~Module() = default;

  Module(const Module&) = delete;
  Module& operator=(const Module&) = delete;

  void redirectLogger(const std::string& targetLoggerName);

protected:
  logging::Logger& log() const;

private:
  mutable logging::Logger logger_;
};

} // namespace pp

namespace pbr {
using ::pp::Module;
} // namespace pbr
