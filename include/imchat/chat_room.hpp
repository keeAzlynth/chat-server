#pragma once

#include "imchat/database.hpp"
#include "imchat/string_hash.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
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
            perms.can_send_private  = false;
            perms.can_create_group  = false;
            perms.max_message_length = 200;
            perms.max_groups         = 0;
            break;
        case UserLevel::Member:
            perms.can_create_group = true;
            perms.max_groups = 3;
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

struct RoomEvent {
    nlohmann::json payload;
    std::string    target_user_id;
    bool           broadcast{false};
};

struct JoinResult {
    nlohmann::json payload;
    std::string    user_id;
    bool           ok{false};
};

class ChatRoom final {
public:
    explicit ChatRoom(std::size_t message_max_length, std::shared_ptr<Database> db);

    // Auth
    [[nodiscard]] RoomEvent register_user(std::string_view connection_id, std::string_view username,
                           std::string_view password, std::string_view nickname,
                           std::string_view device_name = "");
    [[nodiscard]] RoomEvent login(std::string_view connection_id, std::string_view username,
                   std::string_view password, std::string_view device_name = "");
    [[nodiscard]] RoomEvent resume_session(std::string_view connection_id, std::string_view token);
    [[nodiscard]] RoomEvent get_user_devices(std::string_view connection_id);
    [[nodiscard]] RoomEvent logout(std::string_view connection_id);

    // Room
    [[nodiscard]] JoinResult join(std::string_view connection_id, std::string_view nickname);
    [[nodiscard]] std::optional<RoomEvent> leave(std::string_view connection_id);
    [[nodiscard]] RoomEvent send_message(std::string_view connection_id, std::string_view text);
    [[nodiscard]] RoomEvent send_file_message(std::string_view connection_id, std::string_view text,
                                std::string_view file_url, std::string_view file_name, std::int64_t file_size);
    [[nodiscard]] RoomEvent send_private_message(std::string_view connection_id, std::string_view receiver_id, std::string_view text);
    [[nodiscard]] RoomEvent typing(std::string_view connection_id) const;
    [[nodiscard]] RoomEvent recall_message(std::string_view connection_id, std::int64_t message_id);
    [[nodiscard]] RoomEvent mark_read(std::string_view connection_id, std::int64_t message_id);
    [[nodiscard]] RoomEvent edit_message(std::string_view connection_id, std::int64_t message_id, std::string_view new_text);
    [[nodiscard]] RoomEvent delete_message(std::string_view connection_id, std::int64_t message_id);

    // Pin & Save
    [[nodiscard]] RoomEvent pin_message(std::string_view connection_id, std::int64_t message_id, std::string_view chat_id);
    [[nodiscard]] RoomEvent unpin_message(std::string_view connection_id, std::int64_t message_id, std::string_view chat_id);
    [[nodiscard]] RoomEvent get_pinned_messages(std::string_view connection_id, std::string_view chat_id);
    [[nodiscard]] RoomEvent save_message_to_collection(std::string_view connection_id, std::int64_t message_id, std::string_view chat_id);
    [[nodiscard]] RoomEvent unsave_message_from_collection(std::string_view connection_id, std::int64_t message_id);
    [[nodiscard]] RoomEvent get_saved_messages(std::string_view connection_id);

    // Group
    [[nodiscard]] RoomEvent create_group(std::string_view connection_id, std::string_view group_name);
    [[nodiscard]] RoomEvent join_group(std::string_view connection_id, std::string_view group_id);
    [[nodiscard]] RoomEvent leave_group(std::string_view connection_id, std::string_view group_id);
    [[nodiscard]] RoomEvent send_group_message(std::string_view connection_id, std::string_view group_id, std::string_view text);
    [[nodiscard]] RoomEvent get_group_members(std::string_view connection_id, std::string_view group_id);
    [[nodiscard]] RoomEvent get_group_message_history(std::string_view connection_id, std::string_view group_id, int limit = 50);
    [[nodiscard]] RoomEvent kick_user(std::string_view connection_id, std::string_view target_user_id, std::string_view group_id);
    [[nodiscard]] RoomEvent ban_user(std::string_view connection_id, std::string_view target_user_id, std::string_view group_id);

    // Query
    [[nodiscard]] RoomEvent get_online_users(std::string_view connection_id) const;
    [[nodiscard]] RoomEvent get_user_info(std::string_view connection_id, std::string_view target_user_id) const;
    [[nodiscard]] RoomEvent get_message_history(std::string_view connection_id, int limit = 50, std::int64_t before_ts = 0);
    [[nodiscard]] RoomEvent get_private_message_history(std::string_view connection_id, std::string_view other_user_id, int limit = 50);

    // Search & Schedule (v2.3)
    [[nodiscard]] RoomEvent search_messages(std::string_view connection_id, std::string_view query, int limit = 50);
    [[nodiscard]] RoomEvent schedule_message(std::string_view connection_id, std::string_view text,
                                              std::string_view chat_type, std::string_view chat_id, std::int64_t send_at);

    [[nodiscard]] std::string connection_id_for_user(std::string_view user_id) const;
    [[nodiscard]] nlohmann::json error(ErrorCode code) const noexcept;

private:
    // Hot User struct - frequently accessed fields grouped
    struct User {
        std::int64_t last_active{0};
        std::string user_id;
        std::string username;
        std::string nickname;
        std::string connection_id;
        UserLevel   level{UserLevel::Member};
        std::atomic<bool> logged_in{false};
        std::atomic<bool> online{false};
    };

    struct Group {
        std::int64_t created_at{0};
        StringSet   members;
        StringSet   admins;
        StringSet   banned_users;
        std::string group_id;
        std::string name;
        std::string owner_id;
    };

    using UserPtr = std::shared_ptr<User>;

    // --- Validation helpers ---
    [[nodiscard]] UserPtr find_user_by_connection(std::string_view connection_id) const;
    [[nodiscard]] bool validate_connection(std::string_view connection_id, UserPtr& out_user) const;

    struct ValidatedUser {
        UserPtr user;
        UserPermissions perms;
    };

    [[nodiscard]] std::variant<ValidatedUser, RoomEvent> require_user(std::string_view connection_id) const;
    [[nodiscard]] std::variant<ValidatedUser, RoomEvent> require_user_with_perm(std::string_view connection_id, bool UserPermissions::*perm) const;

    struct GroupRef {
        StringMap<Group>::iterator it;
    };
    [[nodiscard]] std::variant<GroupRef, RoomEvent> require_group(std::string_view group_id);

    [[nodiscard]] std::variant<std::string, RoomEvent> validate_text(std::string_view text, int max_length) const;

    void register_in_maps(UserPtr user, std::string_view connection_id, std::string_view username);
    void remove_from_maps(const UserPtr& user, std::string_view connection_id);

    [[nodiscard]] static nlohmann::json message_to_json(const MessageRecord& msg) noexcept;
    [[nodiscard]] std::optional<MessageRecord> find_message_by_id(std::int64_t message_id) const;

    [[nodiscard]] std::string generate_id(std::string_view prefix);
    [[nodiscard]] static std::string trim(std::string_view value);
    [[nodiscard]] static const char* code_name(ErrorCode code) noexcept;
    [[nodiscard]] static const char* code_message(ErrorCode code) noexcept;
    [[nodiscard]] UserPermissions get_permissions(std::string_view user_id) const;

    // Rate limiting: simple token bucket per connection
    struct RateLimitState {
        int tokens{10};          // Max messages per window
        std::int64_t window_start{0};
    };
    mutable StringMap<RateLimitState> rate_limits_;
    bool check_rate_limit(std::string_view connection_id) const;

    // In-memory message ring buffer (avoids DB hits for recent history)
    static constexpr std::size_t MAX_RECENT_MSGS = 200;
    std::deque<MessageRecord> recent_messages_;
    std::deque<MessageRecord> recent_private_msgs_;
    StringMap<std::deque<MessageRecord>> group_message_caches_;
    static constexpr std::size_t MAX_GROUP_MSGS = 100;

    // --- Data members (hot fields first) ---
    std::shared_ptr<Database> db_;
    mutable std::shared_mutex mutex_;

    // Maps - frequently accessed together
    StringMap<UserPtr> users_by_id_;
    StringMap<UserPtr> users_by_connection_;
    StringMap<UserPtr> users_by_username_;
    StringSet          online_nicknames_;

    StringMap<Group> groups_;
    std::size_t message_max_length_;
};

} // namespace imchat
