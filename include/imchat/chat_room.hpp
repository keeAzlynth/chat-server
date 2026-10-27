#pragma once

#include "imchat/config.hpp"
#include "imchat/database.hpp"
#include "imchat/rate_limiter.hpp"
#include "imchat/string_hash.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace imchat {

enum class ErrorCode : std::uint8_t {
    NicknameTaken,
    RateLimited,
    MessageTooLong,
    InvalidFormat,
    NotJoined,
    UsernameExists,
    InvalidCredentials,
    UserNotFound,
    AlreadyLoggedIn,
    GroupNotFound,
    NotGroupAdmin,
    PermissionDenied,
};

enum class UserLevel : std::uint8_t {
    Guest      = 0,
    Member     = 1,
    VIP        = 2,
    Moderator  = 3,
    Admin      = 4,
    Owner      = 5,
};

struct UserPermissions {
    bool can_send_message{true};
    bool can_send_private{true};
    bool can_create_group{false};
    bool can_manage_group{false};
    bool can_kick_user{false};
    bool can_ban_user{false};
    bool can_change_nickname{true};
    int  max_message_length{500};
    int  max_groups{3};

    [[nodiscard]] static constexpr UserPermissions for_level(UserLevel level) noexcept {
        UserPermissions perms;
        switch (level) {
        case UserLevel::Guest:
            perms.can_send_private   = false;
            perms.can_create_group   = false;
            perms.max_message_length = 200;
            perms.max_groups         = 0;
            break;
        case UserLevel::Member:
            perms.can_create_group = true;
            perms.max_groups       = 3;
            break;
        case UserLevel::VIP:
            perms.max_message_length = 1000;
            perms.max_groups         = 10;
            break;
        case UserLevel::Moderator:
            perms.can_kick_user      = true;
            perms.max_message_length = 2000;
            perms.max_groups         = 20;
            break;
        case UserLevel::Admin:
            perms.can_manage_group   = true;
            perms.can_kick_user      = true;
            perms.can_ban_user       = true;
            perms.max_message_length = 5000;
            perms.max_groups         = 50;
            break;
        case UserLevel::Owner:
            perms.can_create_group   = true;
            perms.can_manage_group   = true;
            perms.can_kick_user      = true;
            perms.can_ban_user       = true;
            perms.max_message_length = 10000;
            perms.max_groups         = -1;
            break;
        }
        return perms;
    }
};

// Routing info for one command result.
//
// The previous design used a single `broadcast` flag plus an optional
// `target_user_id`, which forced group messages to be sprayed to every
// connected client. Now every event states exactly who must receive it:
//
//   ack         – send the payload back to the requesting connection
//   broadcast   – send it to every connected client
//   recipients  – send it to these user ids (all of their devices)
struct RoomEvent {
    nlohmann::json           payload;
    std::vector<std::string> recipients;
    bool                     broadcast{false};
    bool                     ack{true};

    RoomEvent() = default;
    explicit RoomEvent(nlohmann::json p) : payload(std::move(p)) {}
    RoomEvent(nlohmann::json p, std::vector<std::string> rcpt)
        : payload(std::move(p)), recipients(std::move(rcpt)) {}

    [[nodiscard]] static RoomEvent everyone(nlohmann::json p) {
        RoomEvent e{std::move(p)};
        e.broadcast = true;
        return e;
    }
    [[nodiscard]] static RoomEvent silent_to(nlohmann::json p, std::vector<std::string> rcpt) {
        RoomEvent e{std::move(p), std::move(rcpt)};
        e.ack = false;
        return e;
    }
};

struct JoinResult {
    nlohmann::json payload;
    std::string    user_id;
    bool           ok{false};
};

class ChatRoom final {
public:
    ChatRoom(const AppConfig& config, std::shared_ptr<Database> db);

    // ── Auth ─────────────────────────────────────────────
    [[nodiscard]] RoomEvent register_user(std::string_view connection_id, std::string_view username,
                                          std::string_view password, std::string_view nickname,
                                          std::string_view device_name = "");
    [[nodiscard]] RoomEvent login(std::string_view connection_id, std::string_view username,
                                  std::string_view password, std::string_view device_name = "");
    [[nodiscard]] RoomEvent resume_session(std::string_view connection_id, std::string_view token);
    [[nodiscard]] RoomEvent get_user_devices(std::string_view connection_id);
    [[nodiscard]] RoomEvent logout(std::string_view connection_id);

    // ── Room ─────────────────────────────────────────────
    [[nodiscard]] JoinResult join(std::string_view connection_id, std::string_view nickname);
    [[nodiscard]] std::optional<RoomEvent> leave(std::string_view connection_id);
    [[nodiscard]] RoomEvent send_message(std::string_view connection_id, std::string_view text);
    [[nodiscard]] RoomEvent send_file_message(std::string_view connection_id, std::string_view text,
                                              std::string_view file_url, std::string_view file_name,
                                              std::int64_t file_size);
    [[nodiscard]] RoomEvent send_private_message(std::string_view connection_id,
                                                 std::string_view receiver_id, std::string_view text);
    [[nodiscard]] RoomEvent typing(std::string_view connection_id) const;
    [[nodiscard]] RoomEvent recall_message(std::string_view connection_id, std::int64_t message_id);
    [[nodiscard]] RoomEvent mark_read(std::string_view connection_id, std::int64_t message_id);
    [[nodiscard]] RoomEvent edit_message(std::string_view connection_id, std::int64_t message_id,
                                         std::string_view new_text);
    [[nodiscard]] RoomEvent delete_message(std::string_view connection_id, std::int64_t message_id);

    // ── Pin & save ───────────────────────────────────────
    [[nodiscard]] RoomEvent pin_message(std::string_view connection_id, std::int64_t message_id,
                                        std::string_view chat_id);
    [[nodiscard]] RoomEvent unpin_message(std::string_view connection_id, std::int64_t message_id,
                                          std::string_view chat_id);
    [[nodiscard]] RoomEvent get_pinned_messages(std::string_view connection_id, std::string_view chat_id);
    [[nodiscard]] RoomEvent save_message_to_collection(std::string_view connection_id,
                                                       std::int64_t message_id, std::string_view chat_id);
    [[nodiscard]] RoomEvent unsave_message_from_collection(std::string_view connection_id,
                                                           std::int64_t message_id);
    [[nodiscard]] RoomEvent get_saved_messages(std::string_view connection_id);

    // ── Group ────────────────────────────────────────────
    [[nodiscard]] RoomEvent create_group(std::string_view connection_id, std::string_view group_name);
    [[nodiscard]] RoomEvent join_group(std::string_view connection_id, std::string_view group_id);
    [[nodiscard]] RoomEvent leave_group(std::string_view connection_id, std::string_view group_id);
    [[nodiscard]] RoomEvent send_group_message(std::string_view connection_id, std::string_view group_id,
                                               std::string_view text);
    [[nodiscard]] RoomEvent get_group_members(std::string_view connection_id, std::string_view group_id);
    [[nodiscard]] RoomEvent get_group_message_history(std::string_view connection_id,
                                                      std::string_view group_id, int limit = 50);
    [[nodiscard]] RoomEvent list_groups(std::string_view connection_id);
    [[nodiscard]] RoomEvent kick_user(std::string_view connection_id, std::string_view target_user_id,
                                      std::string_view group_id);
    [[nodiscard]] RoomEvent ban_user(std::string_view connection_id, std::string_view target_user_id,
                                     std::string_view group_id);

    // ── Query ────────────────────────────────────────────
    [[nodiscard]] RoomEvent get_online_users(std::string_view connection_id) const;
    [[nodiscard]] RoomEvent get_user_info(std::string_view connection_id,
                                          std::string_view target_user_id) const;
    [[nodiscard]] RoomEvent get_message_history(std::string_view connection_id, int limit = 50,
                                                std::int64_t before_ts = 0);
    [[nodiscard]] RoomEvent get_private_message_history(std::string_view connection_id,
                                                        std::string_view other_user_id, int limit = 50);

    // ── Search & schedule ────────────────────────────────
    [[nodiscard]] RoomEvent search_messages(std::string_view connection_id, std::string_view query,
                                            int limit = 50);
    [[nodiscard]] RoomEvent schedule_message(std::string_view connection_id, std::string_view text,
                                             std::string_view chat_type, std::string_view chat_id,
                                             std::int64_t send_at);

    // ── Session helpers ──────────────────────────────────
    [[nodiscard]] std::vector<std::string> connections_for_user(std::string_view user_id) const;
    [[nodiscard]] std::string connection_id_for_user(std::string_view user_id) const;
    void forget_connection(std::string_view connection_id);

    [[nodiscard]] nlohmann::json error(ErrorCode code) const;

    struct Stats {
        std::size_t users;
        std::size_t online_users;
        std::size_t connections;
        std::size_t groups;
    };
    [[nodiscard]] Stats stats() const;

private:
    // A user may be reachable through several connections at once
    // (phone + browser). `connections` is the source of truth for presence.
    struct User {
        std::string      user_id;
        std::string      username;
        std::string      nickname;
        StringSet        connections;
        UserLevel        level{UserLevel::Member};
        bool             guest{false};
        std::int64_t     last_active{0};

        [[nodiscard]] std::string primary_connection() const {
            return connections.empty() ? std::string{} : *connections.begin();
        }
        [[nodiscard]] bool online() const noexcept { return !connections.empty(); }
    };

    using UserPtr = std::shared_ptr<User>;

    struct Group {
        StringSet    members;
        StringSet    admins;
        StringSet    banned_users;
        std::string  group_id;
        std::string  name;
        std::string  owner_id;
        std::int64_t created_at{0};
    };

    struct UserSnapshot {
        std::string user_id;
        std::string username;
        std::string nickname;
        UserLevel   level{UserLevel::Member};
        bool        guest{false};
    };

    // ── Validation ───────────────────────────────────────
    [[nodiscard]] std::optional<UserSnapshot> snapshot_by_connection(std::string_view connection_id) const;
    [[nodiscard]] std::optional<UserSnapshot> snapshot_by_id(std::string_view user_id) const;
    [[nodiscard]] std::variant<UserSnapshot, RoomEvent> require_user(std::string_view connection_id) const;
    [[nodiscard]] std::variant<UserSnapshot, RoomEvent> require_perm(std::string_view connection_id,
                                                                    bool UserPermissions::*perm) const;
    [[nodiscard]] std::variant<std::string, RoomEvent> validate_text(std::string_view text,
                                                                   int max_length) const;

    // ── Membership bookkeeping (all take the state lock) ──
    void attach_connection(const UserPtr& user, std::string_view connection_id);
    void detach_connection(std::string_view connection_id);
    void load_groups();

    // Copies out what a caller needs so the lock can be released before any
    // database work happens.
    [[nodiscard]] std::optional<Group> group_snapshot(std::string_view group_id) const;
    [[nodiscard]] std::vector<std::string> group_recipients(std::string_view group_id,
                                                            std::string_view except_user) const;

    // ── Helpers ──────────────────────────────────────────
    [[nodiscard]] static nlohmann::json message_to_json(const MessageRecord& msg);
    [[nodiscard]] nlohmann::json message_json(const MessageRecord& msg, std::int64_t message_id) const;
    [[nodiscard]] static std::string trim(std::string_view value);
    [[nodiscard]] static const char* code_name(ErrorCode code) noexcept;
    [[nodiscard]] static const char* code_message(ErrorCode code) noexcept;
    [[nodiscard]] static int clamp_limit(int limit, std::size_t max_limit) noexcept;
    [[nodiscard]] UserPermissions permissions(UserLevel level) const;

    [[nodiscard]] RoomEvent rate_limited() const;
    bool allow_command(std::string_view connection_id) const;

    std::shared_ptr<Database> db_;
    std::size_t               message_max_length_;
    std::size_t               max_history_limit_;

    mutable std::shared_mutex mutex_;          // guards the maps below
    StringMap<UserPtr>        users_by_id_;
    StringMap<UserPtr>        users_by_username_;
    StringMap<std::string>    user_id_by_connection_;
    StringSet                 online_nicknames_;   // guests only

    StringMap<Group>          groups_;

    mutable RateLimiter       command_limiter_;
    mutable RateLimiter       auth_limiter_;
};

} // namespace imchat
