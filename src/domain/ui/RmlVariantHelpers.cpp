#include "domain/ui/RmlVariantHelpers.h"

namespace pbr {

std::optional<int> EventArgAsInt(const ui::VariantList& args, size_t index) {
  if (args.size() <= index) {
    return std::nullopt;
  }
  const ui::Variant& value = args[index];
  switch (value.GetType()) {
  case ui::Variant::INT:
    return value.Get<int>();
  case ui::Variant::INT64:
    return static_cast<int>(value.Get<int64_t>());
  case ui::Variant::FLOAT:
    return static_cast<int>(value.Get<float>());
  case ui::Variant::DOUBLE:
    return static_cast<int>(value.Get<double>());
  case ui::Variant::STRING:
    try {
      return std::stoi(std::string(value.Get<ui::String>().c_str()));
    } catch (...) {
      return std::nullopt;
    }
  default:
    return std::nullopt;
  }
}

std::optional<std::string> EventArgAsString(const ui::VariantList& args, size_t index) {
  if (args.size() <= index || args[index].GetType() != ui::Variant::STRING) {
    return std::nullopt;
  }
  return std::string(args[index].Get<ui::String>().c_str());
}

} // namespace pbr
