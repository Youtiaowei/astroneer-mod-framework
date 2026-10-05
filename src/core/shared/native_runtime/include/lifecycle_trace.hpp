#pragma once
// Observation only. Do not allocate records, schedule cleanup or change admission.
#include <DynamicOutput/DynamicOutput.hpp>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <windows.h>
namespace astro::lifecycle {
inline std::atomic<uint64_t> sequence{0};
inline uint64_t next_instance() noexcept {
  return sequence.fetch_add(1, std::memory_order_relaxed) + 1;
}
inline void emit(uint64_t id, const wchar_t* event, uint64_t value=0) noexcept {
  try {
    wchar_t line[256];
    const int length=_snwprintf_s(line,256,_TRUNCATE,
        L"[AstroNativeLifecycle] instance=%llu event=%ls thread=%lu value=%llu\n",
        static_cast<unsigned long long>(id),event,
        static_cast<unsigned long>(GetCurrentThreadId()),static_cast<unsigned long long>(value));
    if(length>0) RC::Output::send(STR("{}"),line);
  } catch (...) {
    // A missing observation is not success. Never let a logger failure prevent
    // retirement, escape a callback, or change the original shutdown diagnostic.
  }
}
}
