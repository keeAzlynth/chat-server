#include "imchat/chat_room.hpp"
#include "imchat/ids.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>

namespace imchat {
namespace {

std::int64_t unix_seconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Message length is counted in UTF-8 code points, not bytes, so a 500
// character limit means 500 Chinese characters rather than ~166.
std::size_t utf8_length(std::string_view text) noexcept
{
    std::size_t count = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) ++count;
    }
    return count;
}

constexpr std::size_t kMaxUsernameLength  = 64;
constexpr std::size_t kMaxNicknameLength  = 32;
constexpr std::size_t kMaxPasswordLength  = 128;
constexpr std::size_t kMaxGroupNameLength = 32;
constexpr std::size_t kMaxFileUrlLength   = 1024;

bool is_safe_file_url(std::string_view url) noexcept
{
    if (url.empty() || url.size() > kMaxFileUrlLength) return false;
    if (url.find('\n') != std::string_view::npos || url.find('\r') != std::string_view::npos) return false;
    return url.starts_with("/uploads/") || url.starts_with("https://") || url.starts_with("http://");
}

} // namespace

// ── Construction ─────────────────────────────────────────

ChatRoom::ChatRoom(const AppConfig& config, std::shared_ptr<Database> db)
    : db_(std::move(db))
    , message_max_length_(config.message_max_length)
    , max_history_limit_(config.max_history_limit)
    , command_limiter_(config.rate_limit_msgs_per_sec, config.rate_limit_burst)
    , auth_limiter_(config.auth_attempts_per_sec, config.auth_burst)
{
    // Groups used to live only in memory, so every restart silently lost them
    // and join_group answered GROUP_NOT_FOUND for groups that still existed in
    // the database.
    load_groups();
}

void ChatRoom::load_groups()
{
    std::size_t members_loaded = 0;
    for (const auto& [group_id, name, owner_id] : db_->get_all_groups()) {
        Group group;
        group.group_id = group_id;
        group.name     = name;
        group.owner_id = owner_id;

        for (const auto& [user_id, role] : db_->get_group_members(group_id)) {
            group.members.insert(user_id);
            if (role == "admin" || role == "owner" || user_id == owner_id) {
                group.admins.insert(user_id);
            }
            ++members_loaded;
        }

        groups_.emplace(group_id, std::move(group));
    }

    if (!groups_.empty()) {
        spdlog::info("loaded {} group(s) / {} membership(s) from database",
                     groups_.size(), members_loaded);
    }
}

// ── Small helpers ────────────────────────────────────────

std::string ChatRoom::trim(std::string_view value)
{
    const auto start = value.find_first_not_of(" \t\n\r");
    if (start == std::string_view::npos) return {};
    const auto end = value.find_last_not_of(" \t\n\r");
    return std::string(value.substr(start, end - start + 1));
}

int ChatRoom::clamp_limit(int limit, std::size_t max_limit) noexcept
{
    if (limit <= 0) return 1;
    const auto upper = static_cast<int>(max_limit);
    return std::min(limit, upper);
}

// 配置里的 message_max_length 是「普通成员」的基准值，其余等级按倍数放大。
// 之前这个配置项被硬编码的等级表覆盖，改了也没用。
UserPermissions ChatRoom::permissions(UserLevel level) const
{
    auto perms = UserPermissions::for_level(level);
    const int base = static_cast<int>(message_max_length_ > 0 ? message_max_length_ : 500);

    switch (level) {
    case UserLevel::Guest:     perms.max_message_length = std::min(200, base); break;
    case UserLevel::Member:    perms.max_message_length = base;                break;
    case UserLevel::VIP:       perms.max_message_length = base * 2;            break;
    case UserLevel::Moderator: perms.max_message_length = base * 4;            break;
    case UserLevel::Admin:     perms.max_message_length = base * 10;           break;
    case UserLevel::Owner:     perms.max_message_length = base * 20;           break;
    }
    return perms;
}

std::variant<std::string, RoomEvent> ChatRoom::validate_text(std::string_view text, int max_length) const
{
    auto clean = trim(text);
    if (clean.empty()) return RoomEvent{error(ErrorCode::InvalidFormat)};

    const auto effective_max = static_cast<std::size_t>(max_length > 0 ? max_length : 1);
    if (utf8_length(clean) > effective_max) return RoomEvent{error(ErrorCode::MessageTooLong)};
    return clean;
}

std::optional<ChatRoom::UserSnapshot> ChatRoom::snapshot_by_connection(std::string_view connection_id) const
{
    std::shared_lock lock(mutex_);

    auto conn_it = user_id_by_connection_.find(connection_id);
    if (conn_it == user_id_by_connection_.end()) return std::nullopt;

    auto user_it = users_by_id_.find(conn_it->second);
    if (user_it == users_by_id_.end()) return std::nullopt;

    const auto& user = *user_it->second;
    return UserSnapshot{user.user_id, user.username, user.nickname, user.level, user.guest};
}

std::optional<ChatRoom::UserSnapshot> ChatRoom::snapshot_by_id(std::string_view user_id) const
{
    std::shared_lock lock(mutex_);
    auto it = users_by_id_.find(user_id);
    if (it == users_by_id_.end()) return std::nullopt;

    const auto& user = *it->second;
    return UserSnapshot{user.user_id, user.username, user.nickname, user.level, user.guest};
}

std::variant<ChatRoom::UserSnapshot, RoomEvent> ChatRoom::require_user(std::string_view connection_id) const
{
    auto snapshot = snapshot_by_connection(connection_id);
    if (!snapshot) return RoomEvent{error(ErrorCode::NotJoined)};
    return *snapshot;
}

std::variant<ChatRoom::UserSnapshot, RoomEvent> ChatRoom::require_perm(
    std::string_view connection_id, bool UserPermissions::*perm) const
{
    auto result = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&result)) return *err;

    const auto& user = std::get<UserSnapshot>(result);
    if (!(permissions(user.level).*perm)) {
        return RoomEvent{error(ErrorCode::PermissionDenied)};
    }
    return user;
}

std::optional<ChatRoom::Group> ChatRoom::group_snapshot(std::string_view group_id) const
{
    std::shared_lock lock(mutex_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) return std::nullopt;
    return it->second;
}

std::vector<std::string> ChatRoom::group_recipients(std::string_view group_id,
                                                    std::string_view except_user) const
{
    std::vector<std::string> out;
    std::shared_lock lock(mutex_);

    auto it = groups_.find(group_id);
    if (it == groups_.end()) return out;

    out.reserve(it->second.members.size());
    for (const auto& member : it->second.members) {
        if (member != except_user) out.push_back(member);
    }
    return out;
}

// Caller must hold the write lock.
void ChatRoom::attach_connection(const UserPtr& user, std::string_view connection_id)
{
    user->connections.insert(std::string(connection_id));
    user->last_active = unix_seconds();

    user_id_by_connection_[std::string(connection_id)] = user->user_id;
    users_by_id_[user->user_id] = user;
    if (!user->username.empty()) users_by_username_[user->username] = user;
    if (user->guest) online_nicknames_.insert(user->nickname);
}

// Caller must hold the write lock.
void ChatRoom::detach_connection(std::string_view connection_id)
{
    auto conn_it = user_id_by_connection_.find(connection_id);
    if (conn_it == user_id_by_connection_.end()) return;

    const std::string user_id = conn_it->second;
    user_id_by_connection_.erase(conn_it);

    auto user_it = users_by_id_.find(user_id);
    if (user_it == users_by_id_.end()) return;

    const auto user = user_it->second;
    user->connections.erase(std::string(connection_id));

    // A user with other live connections stays online — this is what makes
    // "log in on phone and browser" work instead of kicking the first device.
    if (!user->connections.empty()) return;

    if (user->guest) online_nicknames_.erase(user->nickname);
    users_by_id_.erase(user_it);

    if (!user->username.empty()) {
        auto name_it = users_by_username_.find(user->username);
        if (name_it != users_by_username_.end() && name_it->second == user) {
            users_by_username_.erase(name_it);
        }
    }
}

RoomEvent ChatRoom::rate_limited() const
{
    return RoomEvent{error(ErrorCode::RateLimited)};
}

bool ChatRoom::allow_command(std::string_view connection_id) const
{
    return command_limiter_.allow(connection_id);
}

// ── JSON shaping ─────────────────────────────────────────

nlohmann::json ChatRoom::message_to_json(const MessageRecord& msg)
{
    return {
        {"messageId",  msg.id},
        {"userId",     msg.sender_id},
        {"nickname",   msg.sender_nickname},
        {"text",       msg.text},
        {"ts",         msg.timestamp},
        {"type",       msg.message_type},
        {"receiverId", msg.receiver_id},
        {"fileUrl",    msg.file_url},
        {"fileName",   msg.file_name},
        {"fileSize",   msg.file_size},
        {"isRecalled", msg.is_recalled},
    };
}

nlohmann::json ChatRoom::message_json(const MessageRecord& msg, std::int64_t message_id) const
{
    auto json = message_to_json(msg);
    json["messageId"] = message_id;
    return json;
}

// ============================================================
// Auth
// ============================================================

RoomEvent ChatRoom::register_user(std::string_view connection_id, std::string_view username,
                                  std::string_view password, std::string_view nickname,
                                  std::string_view device_name)
{
    if (!auth_limiter_.allow(connection_id)) return rate_limited();

    const auto clean_username = trim(username);
    const auto clean_nickname = trim(nickname);

    if (clean_username.empty() || password.empty() || clean_nickname.empty() ||
        clean_username.size() > kMaxUsernameLength ||
        clean_nickname.size() > kMaxNicknameLength ||
        password.size() > kMaxPasswordLength) {
        return RoomEvent{error(ErrorCode::InvalidFormat)};
    }

    if (snapshot_by_connection(connection_id)) return RoomEvent{error(ErrorCode::AlreadyLoggedIn)};

    // Argon2 hashing happens inside create_user — never while holding our lock.
    if (db_->find_user_by_username(clean_username)) return RoomEvent{error(ErrorCode::UsernameExists)};
    if (!db_->create_user(clean_username, std::string(password), clean_nickname)) {
        return RoomEvent{error(ErrorCode::InvalidFormat)};
    }

    auto db_user = db_->find_user_by_username(clean_username);
    if (!db_user) return RoomEvent{error(ErrorCode::InvalidFormat)};

    const auto token = ids::session_token();
    (void)db_->set_session_token(db_user->user_id, token, ids::device_id(), std::string(device_name));

    {
        std::unique_lock lock(mutex_);
        auto user = std::make_shared<User>();
        user->user_id  = db_user->user_id;
        user->username = db_user->username;
        user->nickname = db_user->nickname;
        user->level    = UserLevel::Member;
        user->guest    = false;
        attach_connection(user, connection_id);
    }

    return RoomEvent{nlohmann::json{
        {"type", "logged_in"},
        {"userId", db_user->user_id},
        {"username", db_user->username},
        {"nickname", db_user->nickname},
        {"level", static_cast<int>(UserLevel::Member)},
        {"sessionToken", token},
    }};
}

RoomEvent ChatRoom::login(std::string_view connection_id, std::string_view username,
                          std::string_view password, std::string_view device_name)
{
    if (!auth_limiter_.allow(connection_id)) return rate_limited();

    const auto clean_username = trim(username);
    if (clean_username.empty() || password.empty() || password.size() > kMaxPasswordLength) {
        return RoomEvent{error(ErrorCode::InvalidFormat)};
    }

    if (snapshot_by_connection(connection_id)) return RoomEvent{error(ErrorCode::AlreadyLoggedIn)};

    auto db_user = db_->authenticate_user(clean_username, std::string(password));
    if (!db_user) return RoomEvent{error(ErrorCode::InvalidCredentials)};

    const auto token = ids::session_token();
    (void)db_->set_session_token(db_user->user_id, token, ids::device_id(), std::string(device_name));

    UserLevel level = UserLevel::Member;
    {
        std::unique_lock lock(mutex_);

        // Multi-device: reuse the in-memory entry if this account is already
        // connected somewhere else, and just add another connection to it.
        auto it = users_by_id_.find(db_user->user_id);
        if (it != users_by_id_.end() && !it->second->connections.empty()) {
            auto& user = it->second;
            user->nickname = db_user->nickname;
            attach_connection(user, connection_id);
            level = user->level;
        } else {
            auto user = std::make_shared<User>();
            user->user_id  = db_user->user_id;
            user->username = db_user->username;
            user->nickname = db_user->nickname;
            user->level    = UserLevel::Member;
            user->guest    = false;
            attach_connection(user, connection_id);
        }
    }

    return RoomEvent{nlohmann::json{
        {"type", "logged_in"},
        {"userId", db_user->user_id},
        {"username", db_user->username},
        {"nickname", db_user->nickname},
        {"level", static_cast<int>(level)},
        {"sessionToken", token},
    }};
}

RoomEvent ChatRoom::resume_session(std::string_view connection_id, std::string_view token)
{
    if (!auth_limiter_.allow(connection_id)) return rate_limited();
    if (token.empty()) return RoomEvent{error(ErrorCode::InvalidCredentials)};

    auto session_info = db_->find_session_by_token(token);
    if (!session_info) return RoomEvent{error(ErrorCode::InvalidCredentials)};

    auto db_user = db_->find_user_by_id(session_info->user_id);
    if (!db_user) return RoomEvent{error(ErrorCode::InvalidCredentials)};

    (void)db_->renew_session_token(token);

    UserLevel level = UserLevel::Member;
    {
        std::unique_lock lock(mutex_);
        auto it = users_by_id_.find(db_user->user_id);
        if (it != users_by_id_.end() && !it->second->connections.empty()) {
            auto& user = it->second;
            user->nickname = db_user->nickname;
            attach_connection(user, connection_id);
            level = user->level;
        } else {
            auto user = std::make_shared<User>();
            user->user_id  = db_user->user_id;
            user->username = db_user->username;
            user->nickname = db_user->nickname;
            user->level    = UserLevel::Member;
            user->guest    = false;
            attach_connection(user, connection_id);
        }
    }

    nlohmann::json response{
        {"type", "logged_in"},
        {"userId", db_user->user_id},
        {"username", db_user->username},
        {"nickname", db_user->nickname},
        {"level", static_cast<int>(level)},
        {"sessionToken", std::string(token)},
        {"resumed", true},
    };
    if (!session_info->device_id.empty()) {
        response["deviceId"]   = session_info->device_id;
        response["deviceName"] = session_info->device_name;
    }
    return RoomEvent{std::move(response)};
}

RoomEvent ChatRoom::get_user_devices(std::string_view connection_id)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    auto sessions = db_->get_user_sessions(user.user_id);
    nlohmann::json devices = nlohmann::json::array();
    for (const auto& s : sessions) {
        devices.push_back({
            {"deviceId", s.device_id},
            {"deviceName", s.device_name},
            {"createdAt", s.created_at},
            {"expiresAt", s.expires_at},
        });
    }
    return RoomEvent{nlohmann::json{
        {"type", "user_devices"},
        {"userId", user.user_id},
        {"devices", std::move(devices)},
    }};
}

RoomEvent ChatRoom::logout(std::string_view connection_id)
{
    std::unique_lock lock(mutex_);
    detach_connection(connection_id);
    return RoomEvent{nlohmann::json{{"type", "logged_out"}}};
}

// ============================================================
// Room
// ============================================================

JoinResult ChatRoom::join(std::string_view connection_id, std::string_view nickname)
{
    const auto clean_nickname = trim(nickname);
    if (clean_nickname.empty() || clean_nickname.size() > kMaxNicknameLength) {
        return {error(ErrorCode::InvalidFormat), {}, false};
    }

    std::unique_lock lock(mutex_);

    auto conn_it = user_id_by_connection_.find(connection_id);
    if (conn_it != user_id_by_connection_.end()) {
        auto user_it = users_by_id_.find(conn_it->second);
        if (user_it != users_by_id_.end()) {
            const auto& user = user_it->second;
            if (user->nickname != clean_nickname) {
                if (user->guest) {
                    if (online_nicknames_.contains(clean_nickname)) {
                        return {error(ErrorCode::NicknameTaken), {}, false};
                    }
                    online_nicknames_.erase(user->nickname);
                    online_nicknames_.insert(clean_nickname);
                }
                user->nickname = clean_nickname;
            }
            user->last_active = unix_seconds();

            users_by_id_[user->user_id] = user;
            return {{
                {"type", "joined"},
                {"userId", user->user_id},
                {"nickname", user->nickname},
                {"level", static_cast<int>(user->level)},
            }, user->user_id, true};
        }
    }

    if (online_nicknames_.contains(clean_nickname)) {
        return {error(ErrorCode::NicknameTaken), {}, false};
    }

    auto user = std::make_shared<User>();
    user->user_id  = ids::user_id(true);
    user->nickname = clean_nickname;
    user->level    = UserLevel::Guest;
    user->guest    = true;
    attach_connection(user, connection_id);

    return {{
        {"type", "joined"},
        {"userId", user->user_id},
        {"nickname", user->nickname},
        {"level", static_cast<int>(UserLevel::Guest)},
    }, user->user_id, true};
}

std::optional<RoomEvent> ChatRoom::leave(std::string_view connection_id)
{
    std::unique_lock lock(mutex_);

    auto conn_it = user_id_by_connection_.find(connection_id);
    if (conn_it == user_id_by_connection_.end()) return std::nullopt;

    const std::string user_id = conn_it->second;
    auto user_it = users_by_id_.find(user_id);
    if (user_it == users_by_id_.end()) return std::nullopt;

    const auto user = user_it->second;
    const std::string nickname = user->nickname;

    detach_connection(connection_id);

    // Still online on another device — the room roster does not change.
    if (!user->connections.empty()) return std::nullopt;

    return RoomEvent::everyone(nlohmann::json{
        {"type", "left"},
        {"userId", user_id},
        {"nickname", nickname},
    });
}

RoomEvent ChatRoom::send_message(std::string_view connection_id, std::string_view text)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    if (!allow_command(connection_id)) return rate_limited();

    const auto perms = permissions(user.level);
    auto tv = validate_text(text, perms.max_message_length);
    if (auto* err = std::get_if<RoomEvent>(&tv)) return *err;
    const auto& clean_text = std::get<std::string>(tv);

    const auto message_id = db_->save_message(user.user_id, user.nickname, "", clean_text, "text");
    if (message_id <= 0) return RoomEvent{error(ErrorCode::InvalidFormat)};

    return RoomEvent::everyone(nlohmann::json{
        {"type", "message"},
        {"messageId", message_id},
        {"userId", user.user_id},
        {"nickname", user.nickname},
        {"text", clean_text},
        {"ts", unix_seconds()},
        {"level", static_cast<int>(user.level)},
    });
}

RoomEvent ChatRoom::send_file_message(std::string_view connection_id, std::string_view text,
                                      std::string_view file_url, std::string_view file_name,
                                      std::int64_t file_size)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    if (!allow_command(connection_id)) return rate_limited();
    if (!is_safe_file_url(file_url)) return RoomEvent{error(ErrorCode::InvalidFormat)};

    const auto perms = permissions(user.level);
    const auto clean_name = trim(file_name);
    auto tv = validate_text(text.empty() ? clean_name : text, perms.max_message_length);
    if (auto* err = std::get_if<RoomEvent>(&tv)) return *err;
    const auto& clean_text = std::get<std::string>(tv);

    const auto size = std::clamp<std::int64_t>(file_size, 0, 1LL << 40);
    const auto message_id = db_->save_message(user.user_id, user.nickname, "", clean_text, "file",
                                              std::string(file_url), clean_name, size);
    if (message_id <= 0) return RoomEvent{error(ErrorCode::InvalidFormat)};

    return RoomEvent::everyone(nlohmann::json{
        {"type", "message"},
        {"messageId", message_id},
        {"userId", user.user_id},
        {"nickname", user.nickname},
        {"text", clean_text},
        {"messageType", "file"},
        {"fileUrl", std::string(file_url)},
        {"fileName", clean_name},
        {"fileSize", size},
        {"ts", unix_seconds()},
    });
}

RoomEvent ChatRoom::send_private_message(std::string_view connection_id,
                                         std::string_view receiver_id, std::string_view text)
{
    auto vu = require_perm(connection_id, &UserPermissions::can_send_private);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    if (!allow_command(connection_id)) return rate_limited();

    const auto perms = permissions(user.level);
    auto tv = validate_text(text, perms.max_message_length);
    if (auto* err = std::get_if<RoomEvent>(&tv)) return *err;
    const auto& clean_text = std::get<std::string>(tv);

    const auto receiver = snapshot_by_id(receiver_id);
    if (!receiver && !db_->find_user_by_id(receiver_id)) {
        return RoomEvent{error(ErrorCode::UserNotFound)};
    }

    const auto message_id = db_->save_message(user.user_id, user.nickname,
                                              std::string(receiver_id), clean_text, "private");
    if (message_id <= 0) return RoomEvent{error(ErrorCode::InvalidFormat)};

    // Only the two parties see a private message — `ack` covers the sender.
    return RoomEvent{
        nlohmann::json{
            {"type", "private_message"},
            {"messageId", message_id},
            {"userId", user.user_id},
            {"nickname", user.nickname},
            {"receiverId", std::string(receiver_id)},
            {"text", clean_text},
            {"ts", unix_seconds()},
            {"level", static_cast<int>(user.level)},
        },
        {std::string(receiver_id)},
    };
}

RoomEvent ChatRoom::typing(std::string_view connection_id) const
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    auto event = RoomEvent::everyone(nlohmann::json{
        {"type", "typing"},
        {"userId", user.user_id},
        {"nickname", user.nickname},
    });
    event.ack = false;   // 发送者自己不需要看到"正在输入"
    return event;
}

RoomEvent ChatRoom::recall_message(std::string_view connection_id, std::int64_t message_id)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    if (message_id <= 0) return RoomEvent{error(ErrorCode::InvalidFormat)};
    if (!db_->recall_message(message_id, user.user_id)) {
        return RoomEvent{error(ErrorCode::PermissionDenied)};
    }

    return RoomEvent::everyone(nlohmann::json{
        {"type", "message_recalled"},
        {"messageId", message_id},
        {"userId", user.user_id},
    });
}

RoomEvent ChatRoom::mark_read(std::string_view connection_id, std::int64_t message_id)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    if (message_id <= 0) return RoomEvent{error(ErrorCode::InvalidFormat)};

    (void)db_->record_message_read(message_id, user.user_id);
    (void)db_->mark_chat_read("public", user.user_id, message_id);

    std::string sender_id;
    if (auto msg = db_->find_message_by_id(message_id)) sender_id = msg->sender_id;

    nlohmann::json payload{
        {"type", "message_read"},
        {"messageId", message_id},
        {"userId", user.user_id},
        {"senderId", sender_id},
        {"readAt", unix_seconds()},
    };

    if (sender_id.empty() || sender_id == user.user_id) return RoomEvent{std::move(payload)};
    return RoomEvent{std::move(payload), {sender_id}};
}

RoomEvent ChatRoom::edit_message(std::string_view connection_id, std::int64_t message_id,
                                 std::string_view new_text)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    const auto perms = permissions(user.level);
    auto tv = validate_text(new_text, perms.max_message_length);
    if (auto* err = std::get_if<RoomEvent>(&tv)) return *err;
    const auto& clean_text = std::get<std::string>(tv);

    if (!db_->edit_message(message_id, user.user_id, clean_text)) {
        return RoomEvent{error(ErrorCode::PermissionDenied)};
    }

    return RoomEvent::everyone(nlohmann::json{
        {"type", "message_edited"},
        {"messageId", message_id},
        {"userId", user.user_id},
        {"text", clean_text},
        {"ts", unix_seconds()},
    });
}

RoomEvent ChatRoom::delete_message(std::string_view connection_id, std::int64_t message_id)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    if (!db_->delete_message(message_id, user.user_id)) {
        return RoomEvent{error(ErrorCode::PermissionDenied)};
    }

    return RoomEvent::everyone(nlohmann::json{
        {"type", "message_deleted"},
        {"messageId", message_id},
        {"userId", user.user_id},
    });
}

// ── Pin & save ───────────────────────────────────────────

RoomEvent ChatRoom::pin_message(std::string_view connection_id, std::int64_t message_id,
                                std::string_view chat_id)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    if (!db_->pin_message(message_id, std::string(chat_id), user.user_id)) {
        return RoomEvent{error(ErrorCode::InvalidFormat)};
    }

    return RoomEvent::everyone(nlohmann::json{
        {"type", "message_pinned"},
        {"messageId", message_id},
        {"chatId", std::string(chat_id)},
        {"pinnedBy", user.nickname},
        {"ts", unix_seconds()},
    });
}

RoomEvent ChatRoom::unpin_message(std::string_view connection_id, std::int64_t message_id,
                                  std::string_view chat_id)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;

    if (!db_->unpin_message(message_id, std::string(chat_id))) {
        return RoomEvent{error(ErrorCode::InvalidFormat)};
    }

    return RoomEvent::everyone(nlohmann::json{
        {"type", "message_unpinned"},
        {"messageId", message_id},
        {"chatId", std::string(chat_id)},
    });
}

RoomEvent ChatRoom::get_pinned_messages(std::string_view connection_id, std::string_view chat_id)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;

    auto pinned = db_->get_pinned_messages(std::string(chat_id));
    nlohmann::json array = nlohmann::json::array();
    for (const auto& record : pinned) {
        if (auto msg = db_->find_message_by_id(record.message_id)) {
            auto json = message_to_json(*msg);
            json["pinnedBy"] = record.pinned_by;
            json["pinnedAt"] = record.pinned_at;
            array.push_back(std::move(json));
        }
    }

    return RoomEvent{nlohmann::json{
        {"type", "pinned_messages"},
        {"chatId", std::string(chat_id)},
        {"messages", std::move(array)},
    }};
}

RoomEvent ChatRoom::save_message_to_collection(std::string_view connection_id,
                                               std::int64_t message_id, std::string_view chat_id)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    if (!db_->save_message_to_collection(message_id, std::string(chat_id), user.user_id)) {
        return RoomEvent{error(ErrorCode::InvalidFormat)};
    }

    return RoomEvent{nlohmann::json{
        {"type", "message_saved"},
        {"messageId", message_id},
        {"chatId", std::string(chat_id)},
    }};
}

RoomEvent ChatRoom::unsave_message_from_collection(std::string_view connection_id,
                                                   std::int64_t message_id)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    if (!db_->unsave_message_from_collection(message_id, user.user_id)) {
        return RoomEvent{error(ErrorCode::InvalidFormat)};
    }

    return RoomEvent{nlohmann::json{
        {"type", "message_unsaved"},
        {"messageId", message_id},
    }};
}

RoomEvent ChatRoom::get_saved_messages(std::string_view connection_id)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    auto saved = db_->get_saved_messages(user.user_id);
    nlohmann::json array = nlohmann::json::array();
    for (const auto& record : saved) {
        if (auto msg = db_->find_message_by_id(record.message_id)) {
            auto json = message_to_json(*msg);
            json["savedAt"] = record.saved_at;
            json["chatId"]  = record.chat_id;
            array.push_back(std::move(json));
        }
    }

    return RoomEvent{nlohmann::json{
        {"type", "saved_messages"},
        {"messages", std::move(array)},
    }};
}

// ============================================================
// Groups
// ============================================================

RoomEvent ChatRoom::create_group(std::string_view connection_id, std::string_view group_name)
{
    auto vu = require_perm(connection_id, &UserPermissions::can_create_group);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    const auto clean_name = trim(group_name);
    if (clean_name.empty() || clean_name.size() > kMaxGroupNameLength) {
        return RoomEvent{error(ErrorCode::InvalidFormat)};
    }

    const auto max_groups = permissions(user.level).max_groups;
    {
        std::shared_lock lock(mutex_);
        if (max_groups >= 0) {
            std::size_t owned = 0;
            for (const auto& [id, group] : groups_) {
                if (group.owner_id == user.user_id) ++owned;
            }
            if (owned >= static_cast<std::size_t>(max_groups)) {
                return RoomEvent{error(ErrorCode::PermissionDenied)};
            }
        }
    }

    const auto id = ids::group_id();
    if (!db_->create_group(id, clean_name, user.user_id)) {
        return RoomEvent{error(ErrorCode::InvalidFormat)};
    }
    (void)db_->add_group_member(id, user.user_id);

    {
        std::unique_lock lock(mutex_);
        Group group;
        group.group_id   = id;
        group.name       = clean_name;
        group.owner_id   = user.user_id;
        group.created_at = unix_seconds();
        group.members.insert(user.user_id);
        group.admins.insert(user.user_id);
        groups_[id] = std::move(group);
    }

    return RoomEvent{nlohmann::json{
        {"type", "group_created"},
        {"groupId", id},
        {"name", clean_name},
        {"ownerId", user.user_id},
    }};
}

RoomEvent ChatRoom::join_group(std::string_view connection_id, std::string_view group_id)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    {
        std::unique_lock lock(mutex_);
        auto it = groups_.find(group_id);
        if (it == groups_.end()) return RoomEvent{error(ErrorCode::GroupNotFound)};
        if (it->second.banned_users.contains(user.user_id)) {
            return RoomEvent{error(ErrorCode::PermissionDenied)};
        }
        it->second.members.insert(user.user_id);
    }

    (void)db_->add_group_member(group_id, user.user_id);

    return RoomEvent::everyone(nlohmann::json{
        {"type", "group_member_joined"},
        {"groupId", std::string(group_id)},
        {"userId", user.user_id},
        {"nickname", user.nickname},
    });
}

RoomEvent ChatRoom::leave_group(std::string_view connection_id, std::string_view group_id)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    {
        std::unique_lock lock(mutex_);
        auto it = groups_.find(group_id);
        if (it == groups_.end()) return RoomEvent{error(ErrorCode::GroupNotFound)};
        it->second.members.erase(user.user_id);
        it->second.admins.erase(user.user_id);
    }

    (void)db_->remove_group_member(group_id, user.user_id);

    return RoomEvent::everyone(nlohmann::json{
        {"type", "group_member_left"},
        {"groupId", std::string(group_id)},
        {"userId", user.user_id},
        {"nickname", user.nickname},
    });
}

RoomEvent ChatRoom::send_group_message(std::string_view connection_id, std::string_view group_id,
                                       std::string_view text)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    if (!allow_command(connection_id)) return rate_limited();

    const auto group = group_snapshot(group_id);
    if (!group) return RoomEvent{error(ErrorCode::GroupNotFound)};
    if (!group->members.contains(user.user_id)) return RoomEvent{error(ErrorCode::PermissionDenied)};

    const auto perms = permissions(user.level);
    auto tv = validate_text(text, perms.max_message_length);
    if (auto* err = std::get_if<RoomEvent>(&tv)) return *err;
    const auto& clean_text = std::get<std::string>(tv);

    const auto message_id = db_->save_message(user.user_id, user.nickname, std::string(group_id),
                                              clean_text, "group");
    if (message_id <= 0) return RoomEvent{error(ErrorCode::InvalidFormat)};

    // Only members receive a group message; the sender is covered by `ack`.
    std::vector<std::string> recipients;
    recipients.reserve(group->members.size());
    for (const auto& member : group->members) {
        if (member != user.user_id) recipients.push_back(member);
    }

    return RoomEvent{
        nlohmann::json{
            {"type", "group_message"},
            {"messageId", message_id},
            {"groupId", std::string(group_id)},
            {"userId", user.user_id},
            {"nickname", user.nickname},
            {"text", clean_text},
            {"ts", unix_seconds()},
            {"level", static_cast<int>(user.level)},
        },
        std::move(recipients),
    };
}

RoomEvent ChatRoom::get_group_members(std::string_view connection_id, std::string_view group_id)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;

    const auto group = group_snapshot(group_id);
    if (!group) return RoomEvent{error(ErrorCode::GroupNotFound)};

    std::vector<UserSnapshot> members;
    {
        std::shared_lock lock(mutex_);
        members.reserve(group->members.size());
        for (const auto& member_id : group->members) {
            auto it = users_by_id_.find(member_id);
            if (it == users_by_id_.end()) continue;
            const auto& u = *it->second;
            members.push_back({u.user_id, u.username, u.nickname, u.level, u.guest});
        }
    }

    std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) {
        return a.nickname < b.nickname;
    });

    nlohmann::json array = nlohmann::json::array();
    for (const auto& member : members) {
        array.push_back({
            {"userId", member.user_id},
            {"nickname", member.nickname},
            {"level", static_cast<int>(member.level)},
            {"isAdmin", group->admins.contains(member.user_id)},
            {"isOwner", member.user_id == group->owner_id},
        });
    }

    return RoomEvent{nlohmann::json{
        {"type", "group_members"},
        {"groupId", std::string(group_id)},
        {"members", std::move(array)},
    }};
}

RoomEvent ChatRoom::get_group_message_history(std::string_view connection_id,
                                              std::string_view group_id, int limit)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;

    const auto group = group_snapshot(group_id);
    if (!group) return RoomEvent{error(ErrorCode::GroupNotFound)};

    const auto effective = clamp_limit(limit, max_history_limit_);
    auto messages = db_->get_group_messages(std::string(group_id), effective);

    nlohmann::json history = nlohmann::json::array();
    for (const auto& msg : messages) history.push_back(message_to_json(msg));

    return RoomEvent{nlohmann::json{
        {"type", "group_message_history"},
        {"groupId", std::string(group_id)},
        {"messages", std::move(history)},
    }};
}

RoomEvent ChatRoom::list_groups(std::string_view connection_id)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    auto groups = db_->list_groups();
    nlohmann::json array = nlohmann::json::array();

    for (const auto& group : groups) {
        bool joined = false;
        {
            std::shared_lock lock(mutex_);
            auto it = groups_.find(group.group_id);
            joined = it != groups_.end() && it->second.members.contains(user.user_id);
        }
        array.push_back({
            {"groupId", group.group_id},
            {"name", group.name},
            {"ownerId", group.owner_id},
            {"createdAt", group.created_at},
            {"memberCount", group.member_count},
            {"joined", joined},
            {"lastMessage", group.last_text},
            {"lastTs", group.last_ts},
        });
    }

    return RoomEvent{nlohmann::json{{"type", "group_list"}, {"groups", std::move(array)}}};
}

RoomEvent ChatRoom::kick_user(std::string_view connection_id, std::string_view target_user_id,
                              std::string_view group_id)
{
    auto vu = require_perm(connection_id, &UserPermissions::can_kick_user);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& admin = std::get<UserSnapshot>(vu);

    bool allowed = false;
    {
        std::unique_lock lock(mutex_);
        auto it = groups_.find(group_id);
        if (it == groups_.end()) return RoomEvent{error(ErrorCode::GroupNotFound)};
        auto& group = it->second;
        allowed = group.admins.contains(admin.user_id) || group.owner_id == admin.user_id;
        if (allowed) {
            group.members.erase(std::string(target_user_id));
            group.admins.erase(std::string(target_user_id));
        }
    }
    if (!allowed) return RoomEvent{error(ErrorCode::NotGroupAdmin)};

    (void)db_->remove_group_member(group_id, target_user_id);

    return RoomEvent::everyone(nlohmann::json{
        {"type", "user_kicked"},
        {"groupId", std::string(group_id)},
        {"userId", std::string(target_user_id)},
        {"kickedBy", admin.user_id},
    });
}

RoomEvent ChatRoom::ban_user(std::string_view connection_id, std::string_view target_user_id,
                             std::string_view group_id)
{
    auto vu = require_perm(connection_id, &UserPermissions::can_ban_user);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& admin = std::get<UserSnapshot>(vu);

    bool allowed = false;
    {
        std::unique_lock lock(mutex_);
        auto it = groups_.find(group_id);
        if (it == groups_.end()) return RoomEvent{error(ErrorCode::GroupNotFound)};
        auto& group = it->second;
        allowed = group.admins.contains(admin.user_id) || group.owner_id == admin.user_id;
        if (allowed) {
            std::string target(target_user_id);
            group.members.erase(target);
            group.admins.erase(target);
            group.banned_users.insert(std::move(target));
        }
    }
    if (!allowed) return RoomEvent{error(ErrorCode::NotGroupAdmin)};

    (void)db_->remove_group_member(group_id, target_user_id);

    return RoomEvent::everyone(nlohmann::json{
        {"type", "user_banned"},
        {"groupId", std::string(group_id)},
        {"userId", std::string(target_user_id)},
        {"bannedBy", admin.user_id},
    });
}

// ============================================================
// Queries
// ============================================================

RoomEvent ChatRoom::get_online_users(std::string_view connection_id) const
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;

    struct Entry {
        std::string user_id;
        std::string nickname;
        int         level;
        bool        guest;
    };
    std::vector<Entry> entries;

    {
        std::shared_lock lock(mutex_);
        entries.reserve(users_by_id_.size());
        for (const auto& [id, user] : users_by_id_) {
            if (!user->online()) continue;
            entries.push_back({user->user_id, user->nickname,
                               static_cast<int>(user->level), user->guest});
        }
    }

    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        if (a.level != b.level) return a.level > b.level;
        return a.nickname < b.nickname;
    });

    nlohmann::json users = nlohmann::json::array();
    for (const auto& entry : entries) {
        users.push_back({
            {"userId", entry.user_id},
            {"nickname", entry.nickname},
            {"level", entry.level},
            {"guest", entry.guest},
        });
    }

    return RoomEvent{nlohmann::json{
        {"type", "online_users"},
        {"users", std::move(users)},
        {"count", entries.size()},
    }};
}

RoomEvent ChatRoom::get_user_info(std::string_view connection_id, std::string_view target_user_id) const
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;

    std::optional<UserSnapshot> target = snapshot_by_id(target_user_id);
    bool online = target.has_value();
    std::string username, nickname;
    UserLevel level = UserLevel::Member;

    if (target) {
        username = target->username;
        nickname = target->nickname;
        level    = target->level;
    } else {
        auto record = db_->find_user_by_id(target_user_id);
        if (!record) return RoomEvent{error(ErrorCode::UserNotFound)};
        username = record->username;
        nickname = record->nickname;
    }

    const auto perms = permissions(level);
    return RoomEvent{nlohmann::json{
        {"type", "user_info"},
        {"userId", std::string(target_user_id)},
        {"username", username},
        {"nickname", nickname},
        {"level", static_cast<int>(level)},
        {"online", online},
        {"permissions", {
            {"canSendMessage", perms.can_send_message},
            {"canSendPrivate", perms.can_send_private},
            {"canCreateGroup", perms.can_create_group},
            {"canManageGroup", perms.can_manage_group},
            {"maxMessageLength", perms.max_message_length},
            {"maxGroups", perms.max_groups},
        }},
    }};
}

RoomEvent ChatRoom::get_message_history(std::string_view connection_id, int limit,
                                        std::int64_t before_ts)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;

    const auto effective = clamp_limit(limit, max_history_limit_);
    auto messages = before_ts > 0 ? db_->get_messages_before(before_ts, effective)
                                  : db_->get_recent_messages(effective);

    nlohmann::json history = nlohmann::json::array();
    for (const auto& msg : messages) history.push_back(message_to_json(msg));

    return RoomEvent{nlohmann::json{
        {"type", "message_history"},
        {"messages", std::move(history)},
        {"hasMore", static_cast<int>(messages.size()) >= effective},
    }};
}

RoomEvent ChatRoom::get_private_message_history(std::string_view connection_id,
                                                std::string_view other_user_id, int limit)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    const auto effective = clamp_limit(limit, max_history_limit_);
    auto messages = db_->get_private_messages(user.user_id, std::string(other_user_id), effective);

    nlohmann::json history = nlohmann::json::array();
    for (const auto& msg : messages) history.push_back(message_to_json(msg));

    return RoomEvent{nlohmann::json{
        {"type", "private_message_history"},
        {"otherUserId", std::string(other_user_id)},
        {"messages", std::move(history)},
    }};
}

RoomEvent ChatRoom::search_messages(std::string_view connection_id, std::string_view query, int limit)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;

    const auto clean_query = trim(query);
    if (clean_query.size() < 2) return RoomEvent{error(ErrorCode::InvalidFormat)};

    const auto effective = clamp_limit(limit, max_history_limit_);
    auto results = db_->search_messages(clean_query, effective);

    auto items = nlohmann::json::array();
    for (const auto& r : results) {
        items.push_back({
            {"messageId", r.message_id},
            {"senderId", r.sender_id},
            {"senderNickname", r.sender_nickname},
            {"text", r.text},
            {"chatId", r.receiver_id},
            {"messageType", r.message_type},
            {"timestamp", r.timestamp},
            {"rank", r.rank},
        });
    }

    return RoomEvent{nlohmann::json{
        {"type", "search_results"},
        {"query", clean_query},
        {"results", std::move(items)},
        {"total", static_cast<int>(results.size())},
    }};
}

RoomEvent ChatRoom::schedule_message(std::string_view connection_id, std::string_view text,
                                     std::string_view chat_type, std::string_view chat_id,
                                     std::int64_t send_at)
{
    auto vu = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vu)) return *err;
    const auto& user = std::get<UserSnapshot>(vu);

    const auto perms = permissions(user.level);
    auto tv = validate_text(text, perms.max_message_length);
    if (auto* err = std::get_if<RoomEvent>(&tv)) return *err;

    const auto now = unix_seconds();
    if (send_at <= now || send_at > now + 30LL * 24 * 3600) {
        return RoomEvent{error(ErrorCode::InvalidFormat)};
    }

    const auto id = db_->schedule_message(user.user_id, chat_type, chat_id,
                                          std::get<std::string>(tv), send_at);
    if (id < 0) return RoomEvent{error(ErrorCode::InvalidFormat)};

    return RoomEvent{nlohmann::json{
        {"type", "message_scheduled"},
        {"scheduleId", id},
        {"sendAt", send_at},
    }};
}

// ── Session helpers ──────────────────────────────────────

std::vector<std::string> ChatRoom::connections_for_user(std::string_view user_id) const
{
    std::shared_lock lock(mutex_);
    std::vector<std::string> out;

    auto it = users_by_id_.find(user_id);
    if (it == users_by_id_.end()) return out;

    out.reserve(it->second->connections.size());
    for (const auto& conn : it->second->connections) out.push_back(conn);
    return out;
}

std::string ChatRoom::connection_id_for_user(std::string_view user_id) const
{
    auto connections = connections_for_user(user_id);
    return connections.empty() ? std::string{} : connections.front();
}

void ChatRoom::forget_connection(std::string_view connection_id)
{
    command_limiter_.forget(connection_id);
    auth_limiter_.forget(connection_id);
}

ChatRoom::Stats ChatRoom::stats() const
{
    std::shared_lock lock(mutex_);

    Stats stats{};
    stats.users       = users_by_id_.size();
    stats.groups      = groups_.size();
    stats.connections = user_id_by_connection_.size();
    for (const auto& [id, user] : users_by_id_) {
        if (user->online()) ++stats.online_users;
    }
    return stats;
}

// ============================================================
// Errors
// ============================================================

namespace {

constexpr std::array kCodeNames = std::to_array<std::string_view>({
    "NICKNAME_TAKEN", "RATE_LIMITED", "MESSAGE_TOO_LONG", "INVALID_FORMAT",
    "NOT_JOINED", "USERNAME_EXISTS", "INVALID_CREDENTIALS", "USER_NOT_FOUND",
    "ALREADY_LOGGED_IN", "GROUP_NOT_FOUND", "NOT_GROUP_ADMIN", "PERMISSION_DENIED",
});

constexpr std::array kCodeMessages = std::to_array<std::string_view>({
    "昵称已被占用", "发送过于频繁", "消息超过长度限制", "消息格式错误",
    "请先加入聊天室", "用户名已存在", "用户名或密码错误", "用户不存在",
    "用户已登录", "群组不存在", "不是群管理员", "权限不足",
});

} // namespace

nlohmann::json ChatRoom::error(ErrorCode code) const
{
    const auto index = static_cast<std::size_t>(code);
    const auto safe  = index < kCodeNames.size() ? index : 0;
    return {
        {"type", "error"},
        {"code", kCodeNames[safe]},
        {"message", kCodeMessages[safe]},
    };
}

const char* ChatRoom::code_name(ErrorCode code) noexcept
{
    const auto index = static_cast<std::size_t>(code);
    return kCodeNames[index < kCodeNames.size() ? index : 0].data();
}

const char* ChatRoom::code_message(ErrorCode code) noexcept
{
    const auto index = static_cast<std::size_t>(code);
    return kCodeMessages[index < kCodeMessages.size() ? index : 0].data();
}

} // namespace imchat
