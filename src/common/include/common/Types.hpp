#pragma once

#include <cstddef>
#include <cstdint>

namespace Sailor {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using f32 = float;
using f64 = double;

// Common PlayStation SDK primitives
using SceSize     = std::size_t;
using SceInt32    = std::int32_t;
using SceUInt32   = std::uint32_t;
using SceInt64    = std::int64_t;
using SceUInt64   = std::uint64_t;
using SceUID      = std::int32_t;
using SceMode     = std::uint32_t;

#if defined(__GNUC__) || defined(__clang__)
    #define SAILOR_SYSV_ABI __attribute__((sysv_abi))
    #define SAILOR_MS_ABI   __attribute__((ms_abi))
#else
    #define SAILOR_SYSV_ABI
    #define SAILOR_MS_ABI
#endif

#ifdef _WIN32
    #define SAILOR_EXPORT __declspec(dllexport)
    #define SAILOR_IMPORT __declspec(dllimport)
#else
    #define SAILOR_EXPORT __attribute__((visibility("default")))
    #define SAILOR_IMPORT
#endif

} // namespace Sailor
