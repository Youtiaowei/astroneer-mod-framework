#pragma once
#include "json.hpp"
#include "safe_io.h"
namespace legacy {
// Non-evaluating grammar: return { identifier = literal, ... }. Expressions,
// duplicate keys, executable statements and ambiguous data_root fields refuse.
class Parser {
  const std::string &s;
  size_t p = 0;
  void ws() {
    while (p < s.size()) {
      if (isspace((unsigned char)s[p])) {
        ++p;
        continue;
      }
      if (s.compare(p, 2, "--") == 0) {
        p += 2;
        if (p < s.size() && s[p] == '[') {
          literal();
        } else {
          while (p < s.size() && s[p] != '\n')
            ++p;
        }
        continue;
      }
      break;
    }
  }
  char peek() {
    ws();
    return p < s.size() ? s[p] : 0;
  }
  void take(char c) {
    if (peek() != c)
      throw std::runtime_error("Unsafe legacy configuration");
    ++p;
  }
  std::string id() {
    ws();
    size_t b = p;
    while (p < s.size() && (isalnum((unsigned char)s[p]) || s[p] == '_'))
      ++p;
    if (p == b)
      throw std::runtime_error("Expected identifier");
    return s.substr(b, p - b);
  }
  json::Value literal() {
    auto c = peek();
    if (c == '"') {
      size_t b = p++;
      while (p < s.size()) {
        if (s[p] == '\\') {
          p += 2;
          continue;
        }
        if (s[p++] == '"')
          return json::parse(s.substr(b, p - b));
      }
      throw std::runtime_error("Unclosed string");
    }
    if (c == '[') {
      ++p;
      size_t b = p;
      while (p < s.size() && s[p] == '=')
        ++p;
      std::string eq = s.substr(b, p - b);
      if (eq.size() > 8 || p >= s.size() || s[p++] != '[')
        throw std::runtime_error("Bad long string");
      auto end = s.find("]" + eq + "]", p);
      if (end == s.npos)
        throw std::runtime_error("Unclosed long string");
      auto v = s.substr(p, end - p);
      p = end + eq.size() + 2;
      if (v.find_first_of("\r\n") != v.npos)
        throw std::runtime_error("Multiline data literal refused");
      return v;
    }
    if (c == '{') {
      take('{');
      json::Array a;
      while (peek() != '}') {
        if (a.size() > 32)
          throw std::runtime_error("List limit");
        a.push_back(literal());
        if (peek() != ',')
          break;
        take(',');
      }
      take('}');
      return a;
    }
    auto word = id();
    if (word == "true")
      return true;
    if (word == "false")
      return false;
    throw std::runtime_error("Expressions refused in legacy config");
  }

public:
  explicit Parser(const std::string &text) : s(text) {
    if (s.size() > 8192)
      throw std::runtime_error("Legacy config size");
    io::wide(s);
  }
  std::string root() {
    if (id() != "return")
      throw std::runtime_error("Legacy config must return data");
    take('{');
    json::Object o;
    while (peek() != '}') {
      auto k = id();
      if (k != "data_root" && k != "development" && k != "presentation" && k != "installed_modules")
        throw std::runtime_error("Unknown legacy config key");
      take('=');
      auto v = literal();
      if (!o.emplace(k, v).second)
        throw std::runtime_error("Duplicate legacy key");
      if (peek() != ',' && peek() != ';')
        break;
      ++p;
    }
    take('}');
    if (peek())
      throw std::runtime_error("Trailing legacy code");
    auto r = o.at("data_root").as_string();
    io::absolute(r);
    return r;
  }
};
inline void preferences(const io::Bytes &b) {
  if (b.size() > 4096)
    throw std::runtime_error("Preference size");
  auto v = json::parse(io::text(b));
  auto schema = v["schema"].as_int();
  if (schema == 1) {
    v["enabled"].as_bool();
  } else if (schema == 2) {
    for (auto &[id, x] : v["modules"].as_object()) {
      if (id != "universal_slot")
        throw std::runtime_error("Unknown preference module");
      x.as_bool();
    }
  } else
    throw std::runtime_error("Preference schema");
  if (v.has("seq") && v["seq"].as_int64() < 0)
    throw std::runtime_error("Preference sequence");
  if (v.has("visible"))
    v["visible"].as_bool();
}
} // namespace legacy
