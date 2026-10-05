// Strict bounded adapter to pinned nlohmann/json v3.11.3; license in vendor.
#pragma once
#include "vendor/nlohmann_json.hpp"
#include <set>
#include <variant>
namespace json {
struct Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;
struct Value : std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object> {
  using variant::variant;
  Value() : variant(nullptr) {}
  Value(int v) : variant(int64_t(v)) {}
  Value(unsigned v) : variant(int64_t(v)) {}
  Value(long v) : variant(int64_t(v)) {}
  Value(const char *v) : variant(std::string(v)) {}
  bool is_null() const { return std::holds_alternative<std::nullptr_t>(*this); }
  bool is_bool() const { return std::holds_alternative<bool>(*this); }
  bool is_number() const { return std::holds_alternative<double>(*this) || is_integer(); }
  bool is_integer() const { return std::holds_alternative<int64_t>(*this); }
  bool is_string() const { return std::holds_alternative<std::string>(*this); }
  bool is_array() const { return std::holds_alternative<Array>(*this); }
  bool is_object() const { return std::holds_alternative<Object>(*this); }
  bool as_bool() const { return std::get<bool>(*this); }
  double as_number() const {
    return is_integer() ? double(std::get<int64_t>(*this)) : std::get<double>(*this);
  }
  int64_t as_int64() const { return std::get<int64_t>(*this); }
  int as_int() const {
    auto n = as_int64();
    if (n < INT_MIN || n > INT_MAX)
      throw std::runtime_error("integer range");
    return int(n);
  }
  const std::string &as_string() const { return std::get<std::string>(*this); }
  const Array &as_array() const { return std::get<Array>(*this); }
  const Object &as_object() const { return std::get<Object>(*this); }
  const Value &operator[](const std::string &k) const { return as_object().at(k); }
  const Value &operator[](size_t i) const { return as_array().at(i); }
  bool has(const std::string &k) const { return is_object() && as_object().count(k); }
  size_t size() const {
    return is_array() ? as_array().size() : is_object() ? as_object().size() : 0;
  }
};
inline Value convert(const nlohmann::json &v) {
  if (v.is_null())
    return nullptr;
  if (v.is_boolean())
    return v.get<bool>();
  if (v.is_number_unsigned()) {
    auto n = v.get<uint64_t>();
    if (n > INT64_MAX)
      throw std::runtime_error("integer overflow");
    return int64_t(n);
  }
  if (v.is_number_integer())
    return v.get<int64_t>();
  if (v.is_number_float())
    return v.get<double>();
  if (v.is_string())
    return v.get<std::string>();
  if (v.is_array()) {
    Array a;
    for (auto &x : v)
      a.push_back(convert(x));
    return a;
  }
  Object o;
  for (auto i = v.begin(); i != v.end(); ++i)
    o.emplace(i.key(), convert(i.value()));
  return o;
}
inline Value parse(const std::string &s) {
  if (s.size() > 1024 * 1024)
    throw std::runtime_error("JSON size limit");
  std::map<int, std::set<std::string>> keys;
  auto cb = [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json &v) {
    if (depth > 32)
      throw std::runtime_error("JSON depth limit");
    if (event == nlohmann::json::parse_event_t::object_start)
      keys[depth + 1].clear();
    if (event == nlohmann::json::parse_event_t::key &&
        !keys[depth].insert(v.get<std::string>()).second)
      throw std::runtime_error("Duplicate JSON key");
    return true;
  };
  return convert(nlohmann::json::parse(s, cb, true, false));
}
inline nlohmann::json native(const Value &v) {
  if (v.is_null())
    return nullptr;
  if (v.is_bool())
    return v.as_bool();
  if (v.is_integer())
    return v.as_int64();
  if (v.is_number()) {
    if (!std::isfinite(v.as_number()))
      throw std::runtime_error("non-finite JSON");
    return v.as_number();
  }
  if (v.is_string())
    return v.as_string();
  if (v.is_array()) {
    auto a = nlohmann::json::array();
    for (auto &x : v.as_array())
      a.push_back(native(x));
    return a;
  }
  auto o = nlohmann::json::object();
  for (auto &[k, x] : v.as_object())
    o[k] = native(x);
  return o;
}
inline std::string serialize(const Value &v) { return native(v).dump(); }
inline std::string escape_string(const std::string &s) { return nlohmann::json(s).dump(); }
} // namespace json
