#pragma once

#include <sodium.h>

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace imchat::ids {

inline void ensure_init()
{
    static std::once_flag once;
    std::call_once(once, [] {
        const int rc = sodium_init();
        (void)rc;   // negative only if libsodium cannot initialise at all
    });
}

// Hex encoding of cryptographically strong random bytes.
//
// The original code generated ids with a 32-bit `mt19937_64` draw formatted
// as "%08llx": collisions become likely in the tens of thousands of messages,
// and the same helper was used for session tokens, so tokens were guessable.
[[nodiscard]] inline std::string random_hex(std::size_t bytes)
{
    ensure_init();
    std::vector<unsigned char> raw(bytes);
    randombytes_buf(raw.data(), raw.size());

    std::string out(bytes * 2 + 1, '\0');
    sodium_bin2hex(out.data(), out.size(), raw.data(), raw.size());
    out.resize(bytes * 2);
    return out;
}

[[nodiscard]] inline std::string prefixed(std::string_view prefix, std::size_t bytes = 12)
{
    std::string out;
    out.reserve(prefix.size() + bytes * 2);
    out.append(prefix);
    out.append(random_hex(bytes));
    return out;
}

[[nodiscard]] inline std::string session_token()  { return prefixed("tok_", 32); }
[[nodiscard]] inline std::string device_id()      { return prefixed("dev_", 8); }
[[nodiscard]] inline std::string user_id(bool guest) { return prefixed(guest ? "g_" : "u_", 12); }
[[nodiscard]] inline std::string group_id()       { return prefixed("grp_", 8); }

} // namespace imchat::ids
