#pragma once

#ifdef ASTRO_MATH_EXPORT
#define ASTRO_CORE_API __declspec(dllexport)
#else
#define ASTRO_CORE_API
#endif

// -1 denotes an invalid slot reference; the host binding returns nil for it.
extern "C" ASTRO_CORE_API int astro_slot_tier(int tier, int depth, int index, int count) noexcept;
// NaN denotes invalid dimensions; callers must preserve the existing safe fallback.
extern "C" ASTRO_CORE_API double astro_fit_body(double x, double y, double z, double slot_scale, int tier) noexcept;
