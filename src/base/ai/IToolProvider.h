#pragma once

#include "base/ai/ToolDescriptor.h"

#include <string>
#include <vector>

namespace pbr {

// In-process MCP-shaped capability source: list tools with schema + meta.
// App/feature code registers providers into ToolRegistry at configure time.
class IToolProvider {
public:
  virtual ~IToolProvider() = default;

  virtual std::string Id() const = 0;
  virtual std::vector<ToolDescriptor> ListTools() = 0;
};

} // namespace pbr
