#pragma once
// Private first-party module contract; not a public cross-compiler SDK.
// The metadata gate is POD. Engine/Value share the identical pinned /MD C++ ABI.
// Hot events use copied Object values directly; no per-property JSON bridge.
#include "engine.hpp"
#include <cstddef>
#include <type_traits>

#define ASTRO_STRINGIFY_DETAIL(x) #x
#define ASTRO_STRINGIFY(x) ASTRO_STRINGIFY_DETAIL(x)
namespace astro::module {
inline constexpr uint32_t protocol = 1;
inline constexpr char fingerprint[] =
    "astro-engine:34e8ecfd81cdf966f9c8f0c7b9a820245596e50dc275a7638108e1cdc19a7614;"
    "contract:1;arch:x64;crt:MD;msc:" ASTRO_STRINGIFY(_MSC_FULL_VER)
    ";stl:" ASTRO_STRINGIFY(_MSVC_STL_VERSION)
    ";stl-update:" ASTRO_STRINGIFY(_MSVC_STL_UPDATE)
    ";iterator:" ASTRO_STRINGIFY(_ITERATOR_DEBUG_LEVEL);

enum class Operation : uint32_t {
  snapshot = 1, start_world, end_world, set_enabled, tick,
  object_created, observe_placement, slot_changed, prepare_release,
  request_refresh, refresh
};
// 0 success, 1 standard exception, 2 non-standard exception, 3 bad input.
// out is caller-owned and may be null except for snapshot. Module allocation
// and destruction are always paired in the DLL. No exception may cross this table.
struct Api {
  uint32_t size = 0;
  uint32_t version = 0;
  const char *id = nullptr;       // borrowed immutable NUL strings in retained DLL image
  const char *module_version = nullptr;
  void *(*create)(Engine *) noexcept = nullptr;
  uint32_t (*destroy)(void *) noexcept = nullptr;
  uint32_t (*call)(void *, Operation, Object, bool, Value *) noexcept = nullptr;
};
using Query = uint32_t (*)(uint32_t, const char *, Api *, uint32_t) noexcept;
inline constexpr char export_name[] = "astro_module_query_v1";
static_assert(std::is_standard_layout_v<Api> && std::is_trivially_copyable_v<Api>);
static_assert(sizeof(void *) == 8);
} // namespace astro::module
#undef ASTRO_STRINGIFY
#undef ASTRO_STRINGIFY_DETAIL
