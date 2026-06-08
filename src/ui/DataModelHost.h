#pragma once

#include <RmlUi/Core/DataModelHandle.h>

#include <RmlUi/Core/Types.h>
#include <functional>
#include <string>
#include <unordered_map>

namespace Rml {
class Context;
class DataModelConstructor;
}

namespace ppbrowser {

using DataModelSetupFn = std::function<void(Rml::DataModelConstructor&)>;

class DataModelHost {
public:
  static DataModelHost& Instance();

  bool Register(Rml::Context* context, const std::string& name, DataModelSetupFn setup);
  Rml::DataModelHandle Get(const std::string& name) const;
  void Dirty(const std::string& model, const std::string& key);
  void Clear();

private:
  DataModelHost() = default;

  struct ModelEntry {
    Rml::DataModelHandle handle;
  };

  std::unordered_map<std::string, ModelEntry> models_;
};

} // namespace ppbrowser
