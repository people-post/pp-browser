#pragma once

#include <ui/data/DataModelHandle.h>

#include <ui/base/Types.h>
#include <functional>
#include <string>
#include <unordered_map>

namespace ui {
class Context;
class DataModelConstructor;
}

namespace pbr {

using DataModelSetupFn = std::function<void(ui::DataModelConstructor&)>;

class DataModelHost {
public:
  static DataModelHost& Instance();

  bool Register(ui::Context* context, const std::string& name, DataModelSetupFn setup);
  ui::DataModelHandle Get(const std::string& name) const;
  void Dirty(const std::string& model, const std::string& key);
  void DirtyAll(const std::string& model);
  void Clear();

private:
  DataModelHost() = default;

  struct ModelEntry {
    ui::DataModelHandle handle;
  };

  std::unordered_map<std::string, ModelEntry> models_;
};

} // namespace pbr
