#include "strict_json.hpp"
#include <charconv>
#include <cmath>
#include <limits>
namespace astro::json {
namespace {
void utf8(std::string &s, unsigned c) {
  if (c < 0x80)
    s += char(c);
  else if (c < 0x800) {
    s += char(0xc0 | (c >> 6));
    s += char(0x80 | (c & 63));
  } else if (c < 0x10000) {
    s += char(0xe0 | (c >> 12));
    s += char(0x80 | ((c >> 6) & 63));
    s += char(0x80 | (c & 63));
  } else {
    s += char(0xf0 | (c >> 18));
    s += char(0x80 | ((c >> 12) & 63));
    s += char(0x80 | ((c >> 6) & 63));
    s += char(0x80 | (c & 63));
  }
}
bool utf8_valid(const std::string &s) {
  for (size_t i = 0; i < s.size();) {
    unsigned c = (unsigned char)s[i++];
    if (c < 128)
      continue;
    int n = 0;
    unsigned minimum = 0;
    if (c >= 0xc2 && c <= 0xdf) {
      n = 1;
      c &= 31;
      minimum = 128;
    } else if (c >= 0xe0 && c <= 0xef) {
      n = 2;
      c &= 15;
      minimum = 2048;
    } else if (c >= 0xf0 && c <= 0xf4) {
      n = 3;
      c &= 7;
      minimum = 65536;
    } else
      return false;
    while (n--) {
      if (i >= s.size())
        return false;
      unsigned b = (unsigned char)s[i++];
      if ((b & 0xc0) != 0x80)
        return false;
      c = (c << 6) | (b & 63);
    }
    if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
      return false;
  }
  return true;
}
class Parser {
  const std::string &s;
  size_t at = 0, nodes = 0;
  [[noreturn]] void fail() const {
    throw std::invalid_argument("Invalid bounded JSON at byte " +
                                std::to_string(at));
  }
  char peek() const { return at < s.size() ? s[at] : 0; }
  void whitespace() {
    while (at < s.size() &&
           (s[at] == ' ' || s[at] == '\t' || s[at] == '\r' || s[at] == '\n'))
      ++at;
  }
  bool take(char c) {
    whitespace();
    if (peek() != c)
      return false;
    ++at;
    return true;
  }
  unsigned hex() {
    if (at + 4 > s.size())
      fail();
    unsigned c = 0;
    for (int n = 0; n < 4; ++n) {
      char b = s[at++];
      c <<= 4;
      if (b >= '0' && b <= '9')
        c += b - '0';
      else if (b >= 'a' && b <= 'f')
        c += b - 'a' + 10;
      else if (b >= 'A' && b <= 'F')
        c += b - 'A' + 10;
      else
        fail();
    }
    return c;
  }
  std::string string() {
    if (!take('"'))
      fail();
    std::string out;
    while (at < s.size()) {
      unsigned char c = s[at++];
      if (c == '"')
        return out;
      if (c < 32)
        fail();
      if (c != '\\') {
        out += char(c);
        continue;
      }
      if (at >= s.size())
        fail();
      switch (s[at++]) {
      case '"':
        out += '"';
        break;
      case '\\':
        out += '\\';
        break;
      case '/':
        out += '/';
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'u': {
        unsigned code = hex();
        if (code >= 0xd800 && code <= 0xdbff) {
          if (at + 2 > s.size() || s[at++] != '\\' || s[at++] != 'u')
            fail();
          unsigned low = hex();
          if (low < 0xdc00 || low > 0xdfff)
            fail();
          code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
        } else if (code >= 0xdc00 && code <= 0xdfff)
          fail();
        utf8(out, code);
        break;
      }
      default:
        fail();
      }
    }
    fail();
  }
  Value number() {
    size_t start = at;
    if (peek() == '-')
      ++at;
    if (peek() == '0')
      ++at;
    else {
      if (peek() < '1' || peek() > '9')
        fail();
      while (peek() >= '0' && peek() <= '9')
        ++at;
    }
    if (peek() == '.') {
      ++at;
      if (peek() < '0' || peek() > '9')
        fail();
      while (peek() >= '0' && peek() <= '9')
        ++at;
    }
    if (peek() == 'e' || peek() == 'E') {
      ++at;
      if (peek() == '+' || peek() == '-')
        ++at;
      if (peek() < '0' || peek() > '9')
        fail();
      while (peek() >= '0' && peek() <= '9')
        ++at;
    }
    double n;
    auto result = std::from_chars(s.data() + start, s.data() + at, n);
    if (result.ec != std::errc{} || result.ptr != s.data() + at ||
        !std::isfinite(n))
      fail();
    return n;
  }
  Value value(int depth) {
    whitespace();
    if (depth > 24 || ++nodes > 4096)
      fail();
    if (peek() == '{') {
      ++at;
      Value::Record r;
      if (take('}'))
        return r;
      do {
        auto key = string();
        if (!take(':') || r.contains(key))
          fail();
        r.emplace(key, value(depth + 1));
        if (take('}'))
          return r;
      } while (take(','));
      fail();
    }
    if (peek() == '[') {
      ++at;
      Value::Array a;
      if (take(']'))
        return a;
      do {
        a.push_back(value(depth + 1));
        if (take(']'))
          return a;
      } while (take(','));
      fail();
    }
    if (peek() == '"')
      return string();
    for (auto pair :
         {std::pair{"true", Value(true)}, std::pair{"false", Value(false)},
          std::pair{"null", Value{}}}) {
      std::string_view literal(pair.first);
      if (s.compare(at, literal.size(), literal) == 0) {
        at += literal.size();
        return pair.second;
      }
    }
    return number();
  }

public:
  explicit Parser(const std::string &text) : s(text) {}
  Value run() {
    auto result = value(0);
    whitespace();
    if (at != s.size())
      fail();
    return result;
  }
};
void quote(std::string &out, const std::string &s) {
  if (!utf8_valid(s))
    throw std::invalid_argument("Cannot encode invalid UTF-8");
  out += '"';
  constexpr char hex[] = "0123456789abcdef";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += char(c);
    } else if (c < 32) {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else
      out += char(c);
  }
  out += '"';
}
void write(std::string &out, const Value &v, int depth, size_t bound) {
  if (depth > 24 || out.size() > bound || v.native)
    throw std::invalid_argument("JSON output bound or native value");
  if (v.null())
    out += "null";
  else if (auto b = std::get_if<bool>(&v.data))
    out += *b ? "true" : "false";
  else if (auto integer = std::get_if<int64_t>(&v.data))
    out += std::to_string(*integer);
  else if (auto n = std::get_if<double>(&v.data)) {
    if (!std::isfinite(*n))
      throw std::invalid_argument("Nonfinite JSON number");
    char buf[64];
    auto r = std::to_chars(buf, buf + 64, *n, std::chars_format::general, 17);
    if (r.ec != std::errc{})
      throw std::runtime_error("JSON numeric encoding failed");
    out.append(buf, r.ptr);
  } else if (auto s = std::get_if<std::string>(&v.data))
    quote(out, *s);
  else if (auto r = std::get_if<Value::Record>(&v.data)) {
    out += '{';
    bool first = true;
    for (auto &[k, value] : *r) {
      if (!first)
        out += ',';
      first = false;
      quote(out, k);
      out += ':';
      write(out, value, depth + 1, bound);
    }
    out += '}';
  } else if (auto a = std::get_if<Value::Array>(&v.data)) {
    out += '[';
    bool first = true;
    for (auto &value : *a) {
      if (!first)
        out += ',';
      first = false;
      write(out, value, depth + 1, bound);
    }
    out += ']';
  } else
    throw std::invalid_argument("Cannot encode an engine object");
  if (out.size() > bound)
    throw std::length_error("JSON output exceeds bound");
}
} // namespace
Value parse(const std::string &s, size_t limit) {
  if (s.size() > limit || !utf8_valid(s))
    throw std::invalid_argument("JSON byte bound or UTF-8 violation");
  return Parser(s).run();
}
std::string encode(const Value &v, size_t bound) {
  std::string out;
  write(out, v, 0, bound);
  return out;
}
} // namespace astro::json
