#pragma once

#include "Types.hpp"
#include <string>
#include <string_view>

namespace Sailor {

class Nid {
public:
    constexpr Nid() = default;
    constexpr explicit Nid(u64 value) : m_value(value) {}
    explicit Nid(std::string_view encodedString);

    constexpr u64 Value() const { return m_value; }
    std::string ToString() const;

    constexpr bool operator==(const Nid& other) const = default;
    constexpr auto operator<=>(const Nid& other) const = default;

    static Nid Compute(std::string_view symbolName);

private:
    u64 m_value{0};
};

} // namespace Sailor
