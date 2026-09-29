#pragma once

// Minimal JSON for the preset layer: layout.json / preset_log JSONL /
// motion_tau.json. Covers the Python code's json.load / json.dumps subset.

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace gtajson {

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Json() = default;
  static Json Null() { return Json(); }
  static Json Bool(bool v);
  static Json Number(double v);
  static Json String(const std::string& v);
  static Json Array();
  static Json Object();

  Type type = Type::Null;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> members;  // order-preserving

  bool IsNull() const { return type == Type::Null; }
  bool IsObject() const { return type == Type::Object; }
  bool IsArray() const { return type == Type::Array; }
  bool IsNumber() const { return type == Type::Number; }
  bool IsString() const { return type == Type::String; }
  bool IsBool() const { return type == Type::Bool; }

  const Json* Find(const std::string& key) const;
  Json& operator[](const std::string& key);
  void Set(const std::string& key, Json value);
  void Push(Json value);

  double AsNumber(double fallback = 0) const {
    return type == Type::Number ? number : fallback;
  }
  std::string AsString(const std::string& fallback = "") const {
    return type == Type::String ? string : fallback;
  }
  bool AsBool(bool fallback = false) const {
    return type == Type::Bool ? boolean : fallback;
  }

  // Parses `text`; returns false and fills `error` on malformed input.
  static bool Parse(const std::string& text, Json* out, std::string* error);
  // Compact serialization.
  std::string Dump() const;
};

}  // namespace gtajson
