#include "base/ui/RmlVariantHelpers.h"

namespace pbr {

std::optional<int> EventArgAsInt(const Rml::VariantList& args, size_t index) {
  if (args.size() <= index) {
    return std::nullopt;
  }
  const Rml::Variant& value = args[index];
  switch (value.GetType()) {
  case Rml::Variant::INT:
    return value.Get<int>();
  case Rml::Variant::INT64:
    return static_cast<int>(value.Get<int64_t>());
  case Rml::Variant::FLOAT:
    return static_cast<int>(value.Get<float>());
  case Rml::Variant::DOUBLE:
    return static_cast<int>(value.Get<double>());
  case Rml::Variant::STRING:
    try {
      return std::stoi(std::string(value.Get<Rml::String>().c_str()));
    } catch (...) {
      return std::nullopt;
    }
  default:
    return std::nullopt;
  }
}

std::optional<std::string> EventArgAsString(const Rml::VariantList& args, size_t index) {
  if (args.size() <= index || args[index].GetType() != Rml::Variant::STRING) {
    return std::nullopt;
  }
  return std::string(args[index].Get<Rml::String>().c_str());
}

} // namespace pbr
