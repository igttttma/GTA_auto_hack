#include "json/json.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace gtajson {

Json Json::Bool(bool v) {
  Json j;
  j.type = Type::Bool;
  j.boolean = v;
  return j;
}

Json Json::Number(double v) {
  Json j;
  j.type = Type::Number;
  j.number = v;
  return j;
}

Json Json::String(const std::string& v) {
  Json j;
  j.type = Type::String;
  j.string = v;
  return j;
}

Json Json::Array() {
  Json j;
  j.type = Type::Array;
  return j;
}

Json Json::Object() {
  Json j;
  j.type = Type::Object;
  return j;
}

const Json* Json::Find(const std::string& key) const {
  if (type != Type::Object) return nullptr;
  for (const auto& [k, v] : members) {
    if (k == key) return &v;
  }
  return nullptr;
}

Json& Json::operator[](const std::string& key) {
  if (type != Type::Object) {
    type = Type::Object;
    members.clear();
  }
  for (auto& [k, v] : members) {
    if (k == key) return v;
  }
  members.emplace_back(key, Json());
  return members.back().second;
}

void Json::Set(const std::string& key, Json value) { (*this)[key] = std::move(value); }

void Json::Push(Json value) {
  if (type != Type::Array) {
    type = Type::Array;
    items.clear();
  }
  items.push_back(std::move(value));
}

namespace {

class Parser {
 public:
  Parser(const std::string& text) : text_(text) {}

  bool Parse(Json* out, std::string* error) {
    SkipWs();
    if (!ParseValue(out)) {
      if (error) *error = "bad json at offset " + std::to_string(pos_);
      return false;
    }
    SkipWs();
    if (pos_ != text_.size()) {
      if (error) *error = "trailing json content at offset " + std::to_string(pos_);
      return false;
    }
    return true;
  }

 private:
  void SkipWs() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' ||
            text_[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool ParseValue(Json* out) {
    if (pos_ >= text_.size()) return false;
    const char c = text_[pos_];
    if (c == '{') return ParseObject(out);
    if (c == '[') return ParseArray(out);
    if (c == '"') {
      std::string s;
      if (!ParseString(&s)) return false;
      *out = Json::String(s);
      return true;
    }
    if (c == 't') return ParseLit("true", Json::Bool(true), out);
    if (c == 'f') return ParseLit("false", Json::Bool(false), out);
    if (c == 'n') return ParseLit("null", Json::Null(), out);
    return ParseNumber(out);
  }

  bool ParseLit(const char* lit, Json value, Json* out) {
    const std::size_t len = std::strlen(lit);
    if (text_.compare(pos_, len, lit) != 0) return false;
    pos_ += len;
    *out = std::move(value);
    return true;
  }

  bool ParseNumber(Json* out) {
    const std::size_t start = pos_;
    if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
    while (pos_ < text_.size() &&
           (std::isdigit(static_cast<unsigned char>(text_[pos_])) ||
            text_[pos_] == '.' || text_[pos_] == 'e' || text_[pos_] == 'E' ||
            text_[pos_] == '+' || text_[pos_] == '-')) {
      ++pos_;
    }
    if (pos_ == start) return false;
    try {
      *out = Json::Number(std::stod(text_.substr(start, pos_ - start)));
    } catch (...) {
      return false;
    }
    return true;
  }

  bool ParseString(std::string* out) {
    if (pos_ >= text_.size() || text_[pos_] != '"') return false;
    ++pos_;
    out->clear();
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c == '\\' && pos_ + 1 < text_.size()) {
        ++pos_;
        const char e = text_[pos_++];
        switch (e) {
          case '"': out->push_back('"'); break;
          case '\\': out->push_back('\\'); break;
          case '/': out->push_back('/'); break;
          case 'n': out->push_back('\n'); break;
          case 't': out->push_back('\t'); break;
          case 'r': out->push_back('\r'); break;
          case 'b': out->push_back('\b'); break;
          case 'f': out->push_back('\f'); break;
          case 'u': {
            if (pos_ + 4 > text_.size()) return false;
            const int code = std::stoi(text_.substr(pos_, 4), nullptr, 16);
            pos_ += 4;
            // encode UTF-8 (BMP only; surrogates rarely appear here)
            if (code < 0x80) {
              out->push_back(static_cast<char>(code));
            } else if (code < 0x800) {
              out->push_back(static_cast<char>(0xC0 | (code >> 6)));
              out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
            } else {
              out->push_back(static_cast<char>(0xE0 | (code >> 12)));
              out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
              out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            break;
          }
          default: return false;
        }
        continue;
      }
      out->push_back(c);
      ++pos_;
    }
    return false;
  }

  bool ParseObject(Json* out) {
    ++pos_;  // {
    Json obj = Json::Object();
    SkipWs();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      *out = std::move(obj);
      return true;
    }
    for (;;) {
      SkipWs();
      std::string key;
      if (!ParseString(&key)) return false;
      SkipWs();
      if (pos_ >= text_.size() || text_[pos_] != ':') return false;
      ++pos_;
      SkipWs();
      Json value;
      if (!ParseValue(&value)) return false;
      obj.members.emplace_back(key, std::move(value));
      SkipWs();
      if (pos_ >= text_.size()) return false;
      if (text_[pos_] == ',') { ++pos_; continue; }
      if (text_[pos_] == '}') { ++pos_; *out = std::move(obj); return true; }
      return false;
    }
  }

  bool ParseArray(Json* out) {
    ++pos_;  // [
    Json arr = Json::Array();
    SkipWs();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      *out = std::move(arr);
      return true;
    }
    for (;;) {
      SkipWs();
      Json value;
      if (!ParseValue(&value)) return false;
      arr.items.push_back(std::move(value));
      SkipWs();
      if (pos_ >= text_.size()) return false;
      if (text_[pos_] == ',') { ++pos_; continue; }
      if (text_[pos_] == ']') { ++pos_; *out = std::move(arr); return true; }
      return false;
    }
  }

  const std::string& text_;
  std::size_t pos_ = 0;
};

void Escape(const std::string& in, std::string* out) {
  for (const char c : in) {
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          *out += buf;
        } else {
          out->push_back(c);
        }
    }
  }
}

void DumpValue(const Json& j, std::string* out) {
  switch (j.type) {
    case Json::Type::Null: *out += "null"; break;
    case Json::Type::Bool: *out += j.boolean ? "true" : "false"; break;
    case Json::Type::Number: {
      if (std::isfinite(j.number)) {
        const double r = std::nearbyint(j.number);
        if (std::fabs(j.number - r) < 1e-9 &&
            std::fabs(j.number) < 1e15) {
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(r));
          *out += buf;
        } else {
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%.10g", j.number);
          *out += buf;
        }
      } else {
        *out += "null";
      }
      break;
    }
    case Json::Type::String:
      out->push_back('"');
      Escape(j.string, out);
      out->push_back('"');
      break;
    case Json::Type::Array: {
      out->push_back('[');
      bool first = true;
      for (const auto& item : j.items) {
        if (!first) *out += ",";
        first = false;
        DumpValue(item, out);
      }
      out->push_back(']');
      break;
    }
    case Json::Type::Object: {
      out->push_back('{');
      bool first = true;
      for (const auto& [k, v] : j.members) {
        if (!first) *out += ",";
        first = false;
        out->push_back('"');
        Escape(k, out);
        *out += "\":";
        DumpValue(v, out);
      }
      out->push_back('}');
      break;
    }
  }
}

}  // namespace

bool Json::Parse(const std::string& text, Json* out, std::string* error) {
  return Parser(text).Parse(out, error);
}

std::string Json::Dump() const {
  std::string out;
  DumpValue(*this, &out);
  return out;
}

}  // namespace gtajson
