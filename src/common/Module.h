#pragma once

#include "log/Logger.h"

#include <string>

namespace pbr {

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

} // namespace pbr
