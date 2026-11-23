#pragma once

#include "common/Types.hpp"
#include <string_view>

namespace Sailor {

namespace Pm4 {

constexpr u32 IT_NOP                    = 0x10;
constexpr u32 IT_SET_BASE               = 0x11;
constexpr u32 IT_CLEAR_STATE            = 0x12;
constexpr u32 IT_INDEX_BUFFER_SIZE      = 0x13;
constexpr u32 IT_DISPATCH_DIRECT        = 0x15;
constexpr u32 IT_DISPATCH_INDIRECT      = 0x16;
constexpr u32 IT_DRAW_INDIRECT          = 0x24;
constexpr u32 IT_DRAW_INDEX_INDIRECT    = 0x25;
constexpr u32 IT_INDEX_BASE             = 0x26;
constexpr u32 IT_DRAW_INDEX_2           = 0x27;
constexpr u32 IT_CONTEXT_CONTROL        = 0x28;
constexpr u32 IT_INDEX_TYPE             = 0x2A;
constexpr u32 IT_DRAW_INDEX_AUTO        = 0x2D;
constexpr u32 IT_NUM_INSTANCES          = 0x2F;
constexpr u32 IT_WRITE_DATA             = 0x37;
constexpr u32 IT_MEM_SEMAPHORE          = 0x39;
constexpr u32 IT_WAIT_REG_MEM           = 0x3C;
constexpr u32 IT_EVENT_WRITE            = 0x46;
constexpr u32 IT_EVENT_WRITE_EOP        = 0x47;
constexpr u32 IT_EVENT_WRITE_EOS        = 0x48;
constexpr u32 IT_RELEASE_MEM            = 0x49;
constexpr u32 IT_SET_CONFIG_REG         = 0x68;
constexpr u32 IT_SET_CONTEXT_REG        = 0x69;
constexpr u32 IT_SET_SH_REG             = 0x76;
constexpr u32 IT_SET_UCONFIG_REG        = 0x79;

// Base offsets for register banks
constexpr u32 CONTEXT_REG_BASE          = 0xA000;
constexpr u32 SH_REG_BASE               = 0x2C00;
constexpr u32 UCONFIG_REG_BASE          = 0xC000;

inline std::string_view GetOpcodeName(u32 op) {
    switch (op) {
        case IT_NOP:                 return "NOP";
        case IT_SET_BASE:            return "SET_BASE";
        case IT_CLEAR_STATE:         return "CLEAR_STATE";
        case IT_INDEX_BUFFER_SIZE:   return "INDEX_BUFFER_SIZE";
        case IT_DISPATCH_DIRECT:     return "DISPATCH_DIRECT";
        case IT_DISPATCH_INDIRECT:   return "DISPATCH_INDIRECT";
        case IT_DRAW_INDIRECT:       return "DRAW_INDIRECT";
        case IT_DRAW_INDEX_INDIRECT: return "DRAW_INDEX_INDIRECT";
        case IT_INDEX_BASE:          return "INDEX_BASE";
        case IT_DRAW_INDEX_2:        return "DRAW_INDEX_2";
        case IT_CONTEXT_CONTROL:     return "CONTEXT_CONTROL";
        case IT_INDEX_TYPE:          return "INDEX_TYPE";
        case IT_DRAW_INDEX_AUTO:     return "DRAW_INDEX_AUTO";
        case IT_NUM_INSTANCES:       return "NUM_INSTANCES";
        case IT_WRITE_DATA:          return "WRITE_DATA";
        case IT_MEM_SEMAPHORE:       return "MEM_SEMAPHORE";
        case IT_WAIT_REG_MEM:        return "WAIT_REG_MEM";
        case IT_EVENT_WRITE:         return "EVENT_WRITE";
        case IT_RELEASE_MEM:         return "RELEASE_MEM";
        case IT_SET_CONFIG_REG:      return "SET_CONFIG_REG";
        case IT_SET_CONTEXT_REG:     return "SET_CONTEXT_REG";
        case IT_SET_SH_REG:          return "SET_SH_REG";
        case IT_SET_UCONFIG_REG:     return "SET_UCONFIG_REG";
        default:                     return "UNKNOWN_PM4";
    }
}

} // namespace Pm4

} // namespace Sailor
