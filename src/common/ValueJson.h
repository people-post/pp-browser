#pragma once

/**
 * Thin helpers for pp::common::Value / Object JSON IO used across pp-browser.
 * Prefer these over ad-hoc parse/dump patterns when migrating off nlohmann.
 */

#include "common/PbrCompat.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

inline Roe<Value> ParseValue(const std::string& json_utf8) {
  return json_io::valueFromJsonString(json_utf8);
}

inline Roe<Object> ParseObject(const std::string& json_utf8) {
  auto parsed = json_io::valueFromJsonString(json_utf8);
  if (!parsed) {
    return parsed.error();
  }
  const Object* object = asObject(*parsed);
  if (!object) {
    return Error("JSON root is not an object");
  }
  return *object;
}

inline std::optional<Object> TryParseObject(const std::string& json_utf8) {
  Object out;
  if (!json_io::objectFromJsonString(out, json_utf8)) {
    return std::nullopt;
  }
  return out;
}

inline std::string DumpJson(const Value& value, int indent = -1) {
  auto encoded = json_io::valueToJsonString(value, indent);
  if (!encoded) {
    return R"({"error":"json_encode_failed"})";
  }
  return *encoded;
}

inline std::string DumpJson(const Object& object, int indent = -1) {
  return json_io::objectToJsonString(object, indent);
}

inline Value ObjectValue(Object object) {
  return Value(std::make_shared<Object>(std::move(object)));
}

inline Value ArrayValue(std::vector<Value> elements) {
  return makeArray(std::move(elements));
}

inline std::optional<std::string> ObjectString(const Object& object,
                                               std::string_view key) {
  return object.getString(std::string(key));
}

inline std::optional<bool> ObjectBool(const Object& object, std::string_view key) {
  return object.getIf<bool>(std::string(key));
}

inline std::optional<int64_t> ObjectInt64(const Object& object, std::string_view key) {
  return object.getIf<int64_t>(std::string(key));
}

inline std::optional<uint64_t> ObjectNonNegInt(const Object& object,
                                               std::string_view key) {
  return object.getNonNegInt(std::string(key));
}

inline const Object* ObjectChild(const Object& object, std::string_view key) {
  return object.getObject(std::string(key));
}

inline const Array* ObjectArray(const Object& object, std::string_view key) {
  return object.getArray(std::string(key));
}

/** Deep-merge overlay into base (objects recurse; other types replace). */
inline void DeepMergeObject(Object& base, const Object& overlay) {
  for (const auto& [key, value] : overlay.fields()) {
    if (const Object* overlay_child = asObject(value)) {
      if (const Object* base_child = base.getObject(key)) {
        Object merged = *base_child;
        DeepMergeObject(merged, *overlay_child);
        base.set(key, merged);
        continue;
      }
      base.set(key, *overlay_child);
      continue;
    }
    base.set(key, value);
  }
}

} // namespace pbr
