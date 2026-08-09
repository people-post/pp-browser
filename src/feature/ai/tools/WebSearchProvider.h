#pragma once

#include "base/ai/IToolProvider.h"
#include "base/data/Config.h"

namespace pbr {

class WebSearchProvider : public IToolProvider {
public:
  explicit WebSearchProvider(SearchConfig config);

  std::string Id() const override;
  std::vector<ToolDescriptor> ListTools() override;

private:
  SearchConfig config_;
};

} // namespace pbr
