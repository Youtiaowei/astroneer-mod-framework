#pragma once
#include "engine.hpp"
namespace astro::json {
Value parse(const std::string &, size_t byte_limit = 8192);
std::string encode(const Value &, size_t byte_limit = 1024 * 1024);
} // namespace astro::json
