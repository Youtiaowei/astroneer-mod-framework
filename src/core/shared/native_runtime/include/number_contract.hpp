#pragma once
#include "module_contract.hpp"
namespace astro::module {
// Optional private same-ABI extension. Old modules do not export this symbol.
// Metadata id is an immutable NUL-terminated string inside the retained DLL
// image; set/query callbacks belong to that same authenticated executable map.
// set(instance,value,initial,out): out is required and caller-owned. initial
// restores saved intent before start_world, without equipment/power admission.
// Interactive calls must enforce the module's own native availability rules.
// 0 = checked reply below; 1/2 = standard/nonstandard fault; 3 = invalid input
// refused without changing prior intent. A code-3 reply need not be populated.
// Success reply: finite value in [0,1000000], bool applied and blocked, exactly
// one true. Applied means accepted intent even if unchanged; blocked must keep
// the prior value and include a canonical reason string. Host adds id/saved/error
// and persists only applied replies. Saving failure does not undo native intent.
// Numeric snapshots contain value/minimum/maximum/step/available/reason. Values
// must be finite, minimum>=0, minimum<maximum<=1000000, value within range and
// 0<step<=maximum-minimum. Step controls user drags, not stored-value quantizing.
struct NumberApi {
    uint32_t size=0,version=0;
    const char* id=nullptr;
    uint32_t (*set)(void*,double,bool,Value*) noexcept=nullptr;
};
using NumberQuery=uint32_t (*)(uint32_t,const char*,NumberApi*,uint32_t) noexcept;
inline constexpr const char* number_export="astro_module_number_query_v1";
static_assert(std::is_standard_layout_v<NumberApi>&&std::is_trivially_copyable_v<NumberApi>);
}
