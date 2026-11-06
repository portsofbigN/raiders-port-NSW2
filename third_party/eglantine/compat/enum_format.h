// Force-included into the standalone shader translator: fmt 12 no longer formats enums implicitly, and the
// translator's log and exception messages format many of them. Any enum without its own formatter prints as its
// underlying number (an enum with an explicit fmt::formatter specialization keeps that one, being more specialized).
#pragma once
#include <type_traits>

#include <fmt/format.h>

template <typename T>
struct fmt::formatter<T, char, std::enable_if_t<std::is_enum_v<T>>> : fmt::formatter<std::underlying_type_t<T>, char> {
    auto format(T v, fmt::format_context &ctx) const {
        return fmt::formatter<std::underlying_type_t<T>, char>::format(static_cast<std::underlying_type_t<T>>(v), ctx);
    }
};
