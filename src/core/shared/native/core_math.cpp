#include "core_math.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

int astro_slot_tier(int tier, int depth, int index, int count) noexcept {
    if (tier < 1 || tier > 4 || depth < 1 || depth > tier || index < 0 || count < 1) return -1;
    const int size = (1 << depth) - 1;
    if (static_cast<std::int64_t>(index) >= static_cast<std::int64_t>(size) * count) return -1;
    index %= size;
    while (index > 0) {
        const int left_size = (1 << (depth - 1)) - 1;
        index -= index <= left_size ? 1 : left_size + 1;
        --tier;
        --depth;
    }
    return tier;
}

double astro_fit_body(double x, double y, double z, double slot_scale, int tier) noexcept {
    const auto positive = [](double n) { return std::isfinite(n) && n > 0; };
    if (!positive(x) || !positive(y) || !positive(z) || !positive(slot_scale) || tier < 2 || tier > 4)
        return std::numeric_limits<double>::quiet_NaN();
    const double growth = std::pow(1.15, tier - 2);
    return std::min({0.40, 120.0 / (2.0 * std::max(x, y) * slot_scale), 300.0 / (2.0 * z * slot_scale)}) * growth;
}
