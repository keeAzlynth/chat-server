#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace imchat {

struct StringHash {
    using is_transparent = void;

    [[nodiscard]] constexpr std::size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }

    [[nodiscard]] constexpr std::size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

struct StringEqual {
    using is_transparent = void;

    [[nodiscard]] constexpr bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
        return lhs == rhs;
    }
};

template<typename Value>
using StringMap = std::unordered_map<std::string, Value, StringHash, StringEqual>;

using StringSet = std::unordered_set<std::string, StringHash, StringEqual>;

} // namespace imchat
