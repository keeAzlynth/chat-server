#include "imchat/chat_room.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <random>

namespace imchat {
namespace {

std::int64_t unix_seconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

thread_local std::mt19937_64 rng{std::random_device{}()};

} // namespace

// --- Construction ---

ChatRoom::ChatRoom(std::size_t message_max_length, std::shared_ptr<Database> db)
    : db_(std::move(db))
    , message_max_length_(message_max_length)
{
}

// --- Validation helpers ---

ChatRoom::UserPtr ChatRoom::find_user_by_connection(std::string_view connection_id) const
{
    auto it = users_by_connection_.find(connection_id);
    return (it != users_by_connection_.end()) ? it->second : nullptr;
}

bool ChatRoom::validate_connection(std::string_view connection_id, UserPtr& out_user) const
{
    out_user = find_user_by_connection(connection_id);
    return out_user && out_user->logged_in;
}

std::variant<ChatRoom::ValidatedUser, RoomEvent> ChatRoom::require_user(std::string_view connection_id) const
{
    UserPtr user;
    if (!validate_connection(connection_id, user)) {
        return RoomEvent{error(ErrorCode::NotJoined)};
    }
    return ValidatedUser{user, get_permissions(user->user_id)};
}

std::variant<ChatRoom::ValidatedUser, RoomEvent> ChatRoom::require_user_with_perm(
    std::string_view connection_id, bool UserPermissions::*perm) const
{
    auto result = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&result)) return *err;

    auto& [user, perms] = std::get<ValidatedUser>(result);
    if (!(perms.*perm)) return RoomEvent{error(ErrorCode::PermissionDenied)};

    return std::move(std::get<ValidatedUser>(result));
}

std::variant<ChatRoom::GroupRef, RoomEvent> ChatRoom::require_group(std::string_view group_id)
{
    auto it = groups_.find(group_id);
    if (it == groups_.end()) return RoomEvent{error(ErrorCode::GroupNotFound)};
    return GroupRef{it};
}

std::variant<std::string, RoomEvent> ChatRoom::validate_text(std::string_view text, int max_length) const
{
    auto clean = trim(text);
    if (clean.empty()) return RoomEvent{error(ErrorCode::InvalidFormat)};
    if (clean.size() > static_cast<std::size_t>(max_length))
        return RoomEvent{error(ErrorCode::MessageTooLong)};
    return std::string{clean};
}

// --- Map management helpers ---

void ChatRoom::register_in_maps(UserPtr user, std::string_view connection_id, std::string_view username)
{
    users_by_id_[user->user_id] = user;
    users_by_connection_.emplace(std::string(connection_id), user);
    if (!username.empty()) {
        users_by_username_.emplace(std::string(username), user);
    }
}

void ChatRoom::remove_from_maps(const UserPtr& user, std::string_view connection_id)
{
    online_nicknames_.erase(user->nickname);
    users_by_username_.erase(user->username);
    users_by_connection_.erase(std::string(connection_id));
    users_by_id_.erase(user->user_id);
}

// --- JSON helpers ---

nlohmann::json ChatRoom::message_to_json(const MessageRecord& msg) noexcept
{
    return {
        {"messageId",  msg.id},
        {"userId",     msg.sender_id},
        {"nickname",   msg.sender_nickname},
        {"text",       msg.text},
        {"ts",         msg.timestamp},
        {"type",       msg.message_type},
        {"fileUrl",    msg.file_url},
        {"fileName",   msg.file_name},
        {"fileSize",   msg.file_size},
        {"isRecalled", msg.is_recalled},
    };
}

std::optional<MessageRecord> ChatRoom::find_message_by_id(std::int64_t message_id) const
{
    return db_->find_message_by_id(message_id);
}

// ── Rate limiting (simple token bucket, 10 msgs / second per connection) ──

bool ChatRoom::check_rate_limit(std::string_view connection_id) const
{
    constexpr int max_tokens = 10;
    constexpr std::int64_t window_seconds = 1;

    auto now = unix_seconds();
    auto& state = rate_limits_[std::string{connection_id}];

    if (now - state.window_start > window_seconds) {
        state.tokens = max_tokens;
        state.window_start = now;
    }

    if (state.tokens <= 0) return false;
    --state.tokens;
    return true;
}

// ── ID & string utilities ────────────────────────────────

std::string ChatRoom::generate_id(std::string_view prefix)
{
    std::uniform_int_distribution<std::uint64_t> dist;
    std::string result;
    result.reserve(prefix.size() + 16);
    result.append(prefix);
    std::array<char, 16> buf;
    auto len = std::snprintf(buf.data(), buf.size(), "%08llx",
                             static_cast<unsigned long long>(dist(rng)));
    result.append(buf.data(), static_cast<std::size_t>(len));
    return result;
}

std::string ChatRoom::trim(std::string_view value) {
    auto start = value.find_first_not_of(" \t\n\r");
    if (start == std::string_view::npos) return {};
    auto end = value.find_last_not_of(" \t\n\r");
    return std::string(value.substr(start, end - start + 1));
}

UserPermissions ChatRoom::get_permissions(std::string_view user_id) const
{
    auto it = users_by_id_.find(user_id);
    if (it == users_by_id_.end()) return UserPermissions::for_level(UserLevel::Guest);
    return UserPermissions::for_level(it->second->level);
}

// ============================================================
// Auth
// ============================================================

RoomEvent ChatRoom::register_user(std::string_view connection_id, std::string_view username,
                                  std::string_view password, std::string_view nickname,
                                  std::string_view device_name)
{
    std::unique_lock lock(mutex_);
    auto clean_username = trim(username);
    auto clean_nickname = trim(nickname);

    if (clean_username.empty() || password.empty() || clean_nickname.empty())
        return {error(ErrorCode::InvalidFormat)};

    if (db_->find_user_by_username(clean_username))
        return {error(ErrorCode::UsernameExists)};

    if (!db_->create_user(clean_username, std::string(password), clean_nickname))
        return {error(ErrorCode::InvalidFormat)};

    auto db_user = db_->find_user_by_username(clean_username);
    if (!db_user) return {error(ErrorCode::InvalidFormat)};

    auto user = std::make_shared<User>();
    user->user_id      = std::move(db_user->user_id);
    user->username      = clean_username;
    user->nickname      = clean_nickname;
    user->connection_id = connection_id;
    user->level         = UserLevel::Member;
    user->logged_in     = true;
    user->online        = true;
    user->last_active   = unix_seconds();

    register_in_maps(user, connection_id, clean_username);

    // Generate session token with device info
    auto token = generate_id("tok_");
    auto did = generate_id("dev_");
    (void)db_->set_session_token(user->user_id, token, did, std::string(device_name));

    return {{
        {"type", "logged_in"},
        {"userId", user->user_id},
        {"username", clean_username},
        {"nickname", clean_nickname},
        {"level", static_cast<int>(user->level)},
        {"sessionToken", token},
    }};
}

RoomEvent ChatRoom::login(std::string_view connection_id, std::string_view username,
                           std::string_view password, std::string_view device_name)
{
    std::unique_lock lock(mutex_);
    auto clean_username = trim(username);

    if (clean_username.empty() || password.empty())
        return {error(ErrorCode::InvalidFormat)};

    if (auto existing = find_user_by_connection(connection_id); existing && existing->logged_in)
        return {error(ErrorCode::AlreadyLoggedIn)};

    auto db_user = db_->authenticate_user(clean_username, std::string(password));
    if (!db_user) return {error(ErrorCode::InvalidCredentials)};

    // v2.2: Multi-device login — allow login even if user is already online on another device
    // (removed the AlreadyLoggedIn check for different connections)

    auto user = std::make_shared<User>();
    user->user_id      = std::move(db_user->user_id);
    user->username      = clean_username;
    user->nickname      = db_user->nickname;
    user->connection_id = connection_id;
    user->level         = UserLevel::Member;
    user->logged_in     = true;
    user->online        = true;
    user->last_active   = unix_seconds();

    register_in_maps(user, connection_id, clean_username);

    // Generate session token with device info
    auto token = generate_id("tok_");
    auto did = generate_id("dev_");
    (void)db_->set_session_token(user->user_id, token, did, std::string(device_name));

    return {{
        {"type", "logged_in"},
        {"userId", user->user_id},
        {"username", clean_username},
        {"nickname", db_user->nickname},
        {"level", static_cast<int>(user->level)},
        {"sessionToken", token},
    }};
}

RoomEvent ChatRoom::resume_session(std::string_view connection_id, std::string_view token)
{
    std::unique_lock lock(mutex_);

    if (token.empty()) return {error(ErrorCode::InvalidCredentials)};

    auto session_info = db_->find_session_by_token(token);
    if (!session_info) return {error(ErrorCode::InvalidCredentials)};

    auto db_user = db_->find_user_by_id(session_info->user_id);
    if (!db_user) return {error(ErrorCode::InvalidCredentials)};

    // v2.2: Multi-device login — allow resume even if another device is online
    (void)db_->renew_session_token(token);

    auto user = std::make_shared<User>();
    user->user_id      = db_user->user_id;
    user->username      = db_user->username;
    user->nickname      = db_user->nickname;
    user->connection_id = connection_id;
    user->level         = UserLevel::Member;
    user->logged_in     = true;
    user->online        = true;
    user->last_active   = unix_seconds();

    register_in_maps(user, connection_id, db_user->username);

    auto response = nlohmann::json{
        {"type", "logged_in"},
        {"userId", user->user_id},
        {"username", db_user->username},
        {"nickname", db_user->nickname},
        {"level", static_cast<int>(user->level)},
        {"sessionToken", std::string(token)},
    };

    // Include device info
    if (!session_info->device_id.empty()) {
        response["deviceId"] = session_info->device_id;
        response["deviceName"] = session_info->device_name;
    }
    return {response};
}

RoomEvent ChatRoom::logout(std::string_view connection_id)
{
    std::unique_lock lock(mutex_);
    auto user = find_user_by_connection(connection_id);
    if (!user) return {error(ErrorCode::NotJoined)};

    remove_from_maps(user, connection_id);
    return {{{"type", "logged_out"}}};
}

// ============================================================
// Room
// ============================================================

JoinResult ChatRoom::join(std::string_view connection_id, std::string_view nickname)
{
    std::unique_lock lock(mutex_);
    auto clean_nickname = trim(nickname);
    if (clean_nickname.empty())
        return {error(ErrorCode::InvalidFormat), {}, false};

    // Existing user re-joining — accept if same nickname, else check conflict
    if (auto user = find_user_by_connection(connection_id)) {
        if (clean_nickname == user->nickname) {
            // Same user, same nickname — just refresh
            user->last_active = unix_seconds();
            return {{
                {"type", "joined"},
                {"userId", user->user_id},
                {"nickname", clean_nickname},
                {"level", static_cast<int>(user->level)},
            }, user->user_id, true};
        }
        // Different nickname — need to check availability
        if (online_nicknames_.count(clean_nickname))
            return {error(ErrorCode::NicknameTaken), {}, false};

        online_nicknames_.erase(user->nickname);
        user->nickname    = clean_nickname;
        user->last_active = unix_seconds();
        online_nicknames_.insert(std::string(clean_nickname));
        return {{
            {"type", "joined"},
            {"userId", user->user_id},
            {"nickname", clean_nickname},
            {"level", static_cast<int>(user->level)},
        }, user->user_id, true};
    }

    // First-time join — check nickname availability
    if (online_nicknames_.count(clean_nickname))
        return {error(ErrorCode::NicknameTaken), {}, false};

    // Guest user
    auto user_id = generate_id("g_");
    auto user = std::make_shared<User>();
    user->user_id      = user_id;
    user->nickname      = clean_nickname;
    user->connection_id = connection_id;
    user->level         = UserLevel::Guest;
    user->logged_in     = true;
    user->online        = true;
    user->last_active   = unix_seconds();

    users_by_id_[user_id] = user;
    users_by_connection_.emplace(std::string(connection_id), user);
    online_nicknames_.insert(std::string(clean_nickname));

    return {{
        {"type", "joined"},
        {"userId", user_id},
        {"nickname", clean_nickname},
        {"level", static_cast<int>(user->level)},
    }, user_id, true};
}

std::optional<RoomEvent> ChatRoom::leave(std::string_view connection_id)
{
    std::unique_lock lock(mutex_);
    auto user = find_user_by_connection(connection_id);
    if (!user) return std::nullopt;

    RoomEvent event{
        {{"type", "left"}, {"userId", user->user_id}, {"nickname", user->nickname}},
        {}, true,
    };
    remove_from_maps(user, connection_id);
    return event;
}

RoomEvent ChatRoom::send_message(std::string_view connection_id, std::string_view text)
{
    std::unique_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) [[unlikely]] return *err;
    auto& [user, perms] = std::get<ValidatedUser>(vr);

    if (!check_rate_limit(connection_id)) [[unlikely]] return {error(ErrorCode::RateLimited)};

    auto tv = validate_text(text, perms.max_message_length);
    if (auto* err = std::get_if<RoomEvent>(&tv)) [[unlikely]] return *err;
    auto& clean_text = std::get<std::string>(tv);

    (void)db_->save_message(user->user_id, user->nickname, "", clean_text, "text");

    // Ring buffer cache
    MessageRecord cached{0, unix_seconds(), user->user_id, user->nickname, std::string(clean_text), "", "text", false};
    recent_messages_.push_back(std::move(cached));
    while (recent_messages_.size() > MAX_RECENT_MSGS) recent_messages_.pop_front();

    return {{
        {"type", "message"},
        {"userId", user->user_id},
        {"nickname", user->nickname},
        {"text", clean_text},
        {"ts", unix_seconds()},
        {"level", static_cast<int>(user->level)},
    }, {}, true};
}

RoomEvent ChatRoom::send_file_message(std::string_view connection_id, std::string_view text,
                                      std::string_view file_url, std::string_view file_name, std::int64_t file_size)
{
    std::unique_lock lock(mutex_);

    auto vr = require_user_with_perm(connection_id, &UserPermissions::can_send_message);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [user, perms] = std::get<ValidatedUser>(vr);

    (void)db_->save_message(user->user_id, user->nickname, "", std::string(text), "file",
                      std::string(file_url), std::string(file_name), file_size);

    // Ring buffer cache
    { MessageRecord cached{0, unix_seconds(), user->user_id, user->nickname, std::string(text), "", "file"};
      cached.file_url = file_url; cached.file_name = file_name; cached.file_size = file_size;
      recent_messages_.push_back(std::move(cached));
      while (recent_messages_.size() > MAX_RECENT_MSGS) recent_messages_.pop_front(); }

    return {{
        {"type", "message"},
        {"userId", user->user_id},
        {"nickname", user->nickname},
        {"text", text},
        {"messageType", "file"},
        {"fileUrl", file_url},
        {"fileName", file_name},
        {"fileSize", file_size},
        {"ts", unix_seconds()},
    }, {}, true};
}

RoomEvent ChatRoom::send_private_message(std::string_view connection_id, std::string_view receiver_id, std::string_view text)
{
    std::unique_lock lock(mutex_);

    auto vr = require_user_with_perm(connection_id, &UserPermissions::can_send_private);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [sender, perms] = std::get<ValidatedUser>(vr);

    auto tv = validate_text(text, perms.max_message_length);
    if (auto* err = std::get_if<RoomEvent>(&tv)) return *err;
    auto& clean_text = std::get<std::string>(tv);

    if (users_by_id_.find(receiver_id) == users_by_id_.end())
        return {error(ErrorCode::UserNotFound)};

    (void)db_->save_message(sender->user_id, sender->nickname, std::string(receiver_id), clean_text, "text");

    // Private message ring buffer cache
    { MessageRecord cached{0, unix_seconds(), sender->user_id, sender->nickname, std::string(clean_text), std::string(receiver_id), "private"};
      recent_private_msgs_.push_back(std::move(cached));
      while (recent_private_msgs_.size() > MAX_RECENT_MSGS) recent_private_msgs_.pop_front(); }

    return {{
        {"type", "private_message"},
        {"userId", sender->user_id},
        {"nickname", sender->nickname},
        {"receiverId", receiver_id},
        {"text", clean_text},
        {"ts", unix_seconds()},
        {"level", static_cast<int>(sender->level)},
    }, std::string(receiver_id), true};
}

RoomEvent ChatRoom::typing(std::string_view connection_id) const
{
    std::shared_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [user, _] = std::get<ValidatedUser>(vr);

    return {{{"type", "typing"}, {"userId", user->user_id}, {"nickname", user->nickname}}};
}

RoomEvent ChatRoom::recall_message(std::string_view connection_id, std::int64_t message_id)
{
    std::unique_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [user, _] = std::get<ValidatedUser>(vr);

    if (!db_->recall_message(message_id, user->user_id))
        return {error(ErrorCode::PermissionDenied)};

    return {{
        {"type", "message_recalled"},
        {"messageId", message_id},
        {"userId", user->user_id},
    }, {}, true};
}

RoomEvent ChatRoom::mark_read(std::string_view connection_id, std::int64_t message_id)
{
    std::unique_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [user, _] = std::get<ValidatedUser>(vr);

    // v2.2: Record read receipt + store cursor
    (void)db_->record_message_read(message_id, user->user_id);
    (void)db_->mark_chat_read("public", user->user_id, message_id);

    // Look up the message to find the sender for notification
    auto msg = db_->find_message_by_id(message_id);
    std::string sender_id = msg ? msg->sender_id : "";

    return {
        {{"type", "message_read"},
         {"messageId", message_id},
         {"userId", user->user_id},
         {"senderId", sender_id},
         {"readAt", unix_seconds()}}
    };
}

RoomEvent ChatRoom::get_user_devices(std::string_view connection_id)
{
    std::unique_lock lock(mutex_);
    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [user, _] = std::get<ValidatedUser>(vr);

    auto sessions = db_->get_user_sessions(user->user_id);
    auto devices = nlohmann::json::array();
    for (const auto& s : sessions) {
        devices.push_back({
            {"deviceId", s.device_id},
            {"deviceName", s.device_name},
            {"createdAt", s.created_at},
            {"expiresAt", s.expires_at},
        });
    }
    return {{{"type", "user_devices"}, {"userId", user->user_id}, {"devices", std::move(devices)}}};
}

RoomEvent ChatRoom::edit_message(std::string_view connection_id, std::int64_t message_id, std::string_view new_text)
{
    std::unique_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [user, perms] = std::get<ValidatedUser>(vr);

    auto tv = validate_text(new_text, perms.max_message_length);
    if (auto* err = std::get_if<RoomEvent>(&tv)) return *err;
    auto& clean_text = std::get<std::string>(tv);

    if (!db_->edit_message(message_id, user->user_id, clean_text))
        return {error(ErrorCode::PermissionDenied)};

    return {{
        {"type", "message_edited"},
        {"messageId", message_id},
        {"userId", user->user_id},
        {"text", clean_text},
        {"ts", unix_seconds()},
    }, {}, true};
}

RoomEvent ChatRoom::delete_message(std::string_view connection_id, std::int64_t message_id)
{
    std::unique_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [user, _] = std::get<ValidatedUser>(vr);

    if (!db_->delete_message(message_id, user->user_id))
        return {error(ErrorCode::PermissionDenied)};

    return {{
        {"type", "message_deleted"},
        {"messageId", message_id},
        {"userId", user->user_id},
    }, {}, true};
}

RoomEvent ChatRoom::pin_message(std::string_view connection_id, std::int64_t message_id, std::string_view chat_id)
{
    std::unique_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [user, _] = std::get<ValidatedUser>(vr);

    if (!db_->pin_message(message_id, std::string(chat_id), user->user_id))
        return {error(ErrorCode::InvalidFormat)};

    return {{
        {"type", "message_pinned"},
        {"messageId", message_id},
        {"chatId", chat_id},
        {"pinnedBy", user->nickname},
        {"ts", unix_seconds()},
    }, {}, true};
}

RoomEvent ChatRoom::unpin_message(std::string_view connection_id, std::int64_t message_id, std::string_view chat_id)
{
    std::unique_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;

    if (!db_->unpin_message(message_id, std::string(chat_id)))
        return {error(ErrorCode::InvalidFormat)};

    return {{
        {"type", "message_unpinned"},
        {"messageId", message_id},
        {"chatId", chat_id},
    }, {}, true};
}

RoomEvent ChatRoom::get_pinned_messages(std::string_view connection_id, std::string_view chat_id)
{
    std::shared_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;

    auto pinned = db_->get_pinned_messages(std::string(chat_id));
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : pinned) {
        if (auto msg = find_message_by_id(p.message_id)) {
            auto j = message_to_json(*msg);
            j["pinnedBy"] = p.pinned_by;
            j["pinnedAt"] = p.pinned_at;
            arr.push_back(std::move(j));
        }
    }

    return {{{"type", "pinned_messages"}, {"chatId", chat_id}, {"messages", arr}}};
}

RoomEvent ChatRoom::save_message_to_collection(std::string_view connection_id, std::int64_t message_id, std::string_view chat_id)
{
    std::unique_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [user, _] = std::get<ValidatedUser>(vr);

    if (!db_->save_message_to_collection(message_id, std::string(chat_id), user->user_id))
        return {error(ErrorCode::InvalidFormat)};

    return {{{"type", "message_saved"}, {"messageId", message_id}, {"chatId", chat_id}}};
}

RoomEvent ChatRoom::unsave_message_from_collection(std::string_view connection_id, std::int64_t message_id)
{
    std::unique_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [user, _] = std::get<ValidatedUser>(vr);

    if (!db_->unsave_message_from_collection(message_id, user->user_id))
        return {error(ErrorCode::InvalidFormat)};

    return {{{"type", "message_unsaved"}, {"messageId", message_id}}};
}

RoomEvent ChatRoom::get_saved_messages(std::string_view connection_id)
{
    std::shared_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [user, _] = std::get<ValidatedUser>(vr);

    auto saved = db_->get_saved_messages(user->user_id);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : saved) {
        if (auto msg = find_message_by_id(s.message_id)) {
            auto j = message_to_json(*msg);
            j["savedAt"] = s.saved_at;
            j["chatId"] = s.chat_id;
            arr.push_back(std::move(j));
        }
    }

    return {{{"type", "saved_messages"}, {"messages", arr}}};
}

// ============================================================
// Group
// ============================================================

RoomEvent ChatRoom::create_group(std::string_view connection_id, std::string_view group_name)
{
    std::unique_lock lock(mutex_);

    auto vr = require_user_with_perm(connection_id, &UserPermissions::can_create_group);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [user, _] = std::get<ValidatedUser>(vr);

    auto group_id = generate_id("grp_");
    auto clean_name = trim(group_name);
    
    Group group;
    group.group_id = group_id;
    group.name     = clean_name;
    group.owner_id = user->user_id;
    group.members.insert(user->user_id);
    group.admins.insert(user->user_id);
    group.created_at = unix_seconds();
    groups_[group_id] = std::move(group);

    // Persist to database
    (void)db_->create_group(group_id, clean_name, user->user_id);
    (void)db_->add_group_member(group_id, user->user_id);

    return {{{"type", "group_created"}, {"groupId", group_id}, {"name", clean_name}, {"ownerId", user->user_id}}};
}

RoomEvent ChatRoom::join_group(std::string_view connection_id, std::string_view group_id)
{
    std::unique_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [user, _] = std::get<ValidatedUser>(vr);

    auto gr = require_group(group_id);
    if (auto* err = std::get_if<RoomEvent>(&gr)) return *err;
    auto& group = std::get<GroupRef>(gr).it->second;

    if (group.banned_users.count(user->user_id))
        return {error(ErrorCode::PermissionDenied)};

    group.members.insert(user->user_id);

    // Persist
    (void)db_->add_group_member(group_id, user->user_id);

    return {{
        {"type", "group_member_joined"},
        {"groupId", group_id},
        {"userId", user->user_id},
        {"nickname", user->nickname},
    }, {}, true};
}

RoomEvent ChatRoom::leave_group(std::string_view connection_id, std::string_view group_id)
{
    std::unique_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [user, _] = std::get<ValidatedUser>(vr);

    auto gr = require_group(group_id);
    if (auto* err = std::get_if<RoomEvent>(&gr)) return *err;
    auto& group = std::get<GroupRef>(gr).it->second;

    group.members.erase(user->user_id);
    group.admins.erase(user->user_id);

    // Persist
    (void)db_->remove_group_member(group_id, user->user_id);

    return {{
        {"type", "group_member_left"},
        {"groupId", group_id},
        {"userId", user->user_id},
        {"nickname", user->nickname},
    }, {}, true};
}

RoomEvent ChatRoom::send_group_message(std::string_view connection_id, std::string_view group_id, std::string_view text)
{
    std::unique_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [user, perms] = std::get<ValidatedUser>(vr);

    auto gr = require_group(group_id);
    if (auto* err = std::get_if<RoomEvent>(&gr)) return *err;
    auto& group = std::get<GroupRef>(gr).it->second;

    if (!group.members.count(user->user_id))
        return {error(ErrorCode::PermissionDenied)};

    auto tv = validate_text(text, perms.max_message_length);
    if (auto* err = std::get_if<RoomEvent>(&tv)) return *err;
    auto& clean_text = std::get<std::string>(tv);

    (void)db_->save_message(user->user_id, user->nickname, std::string(group_id), clean_text, "group");

    // Group message ring buffer cache
    { auto& buf = group_message_caches_[std::string(group_id)];
      MessageRecord cached{0, unix_seconds(), user->user_id, user->nickname, std::string(clean_text), std::string(group_id), "group"};
      buf.push_back(std::move(cached));
      while (buf.size() > MAX_GROUP_MSGS) buf.pop_front(); }

    return {{
        {"type", "group_message"},
        {"groupId", group_id},
        {"userId", user->user_id},
        {"nickname", user->nickname},
        {"text", clean_text},
        {"ts", unix_seconds()},
        {"level", static_cast<int>(user->level)},
    }, {}, true};
}

RoomEvent ChatRoom::get_group_members(std::string_view connection_id, std::string_view group_id)
{
    std::shared_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;

    auto gr = require_group(group_id);
    if (auto* err = std::get_if<RoomEvent>(&gr)) return *err;
    auto& group = std::get<GroupRef>(gr).it->second;

    nlohmann::json members = nlohmann::json::array();
    for (const auto& member_id : group.members) {
        if (auto uit = users_by_id_.find(member_id); uit != users_by_id_.end()) {
            members.push_back({
                {"userId", member_id},
                {"nickname", uit->second->nickname},
                {"level", static_cast<int>(uit->second->level)},
                {"isAdmin", group.admins.count(member_id) > 0},
                {"isOwner", member_id == group.owner_id},
            });
        }
    }

    return {{{"type", "group_members"}, {"groupId", group_id}, {"members", members}}};
}

RoomEvent ChatRoom::get_group_message_history(std::string_view connection_id, std::string_view group_id, int limit)
{
    std::shared_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;

    auto gr = require_group(group_id);
    if (auto* err = std::get_if<RoomEvent>(&gr)) return *err;

    std::vector<MessageRecord> messages;
    // Fast path: ring buffer cache
    auto it = group_message_caches_.find(std::string(group_id));
    if (it != group_message_caches_.end() && !it->second.empty()) {
        auto& buf = it->second;
        std::size_t start = (buf.size() > static_cast<std::size_t>(limit)) ? buf.size() - limit : 0;
        messages.assign(buf.begin() + static_cast<long>(start), buf.end());
    } else {
        messages = db_->get_group_messages(std::string(group_id), limit);
    }
    nlohmann::json history = nlohmann::json::array();
    for (const auto& msg : messages) history.push_back(message_to_json(msg));

    return {{{"type", "group_message_history"}, {"groupId", group_id}, {"messages", history}}};
}

RoomEvent ChatRoom::kick_user(std::string_view connection_id, std::string_view target_user_id, std::string_view group_id)
{
    std::unique_lock lock(mutex_);

    auto vr = require_user_with_perm(connection_id, &UserPermissions::can_kick_user);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [admin, _] = std::get<ValidatedUser>(vr);

    auto gr = require_group(group_id);
    if (auto* err = std::get_if<RoomEvent>(&gr)) return *err;
    auto& group = std::get<GroupRef>(gr).it->second;

    if (!group.admins.count(admin->user_id) && admin->user_id != group.owner_id)
        return {error(ErrorCode::NotGroupAdmin)};

    group.members.erase(std::string(target_user_id));
    group.admins.erase(std::string(target_user_id));

    return {{
        {"type", "user_kicked"},
        {"groupId", group_id},
        {"userId", target_user_id},
        {"kickedBy", admin->user_id},
    }, {}, true};
}

RoomEvent ChatRoom::ban_user(std::string_view connection_id, std::string_view target_user_id, std::string_view group_id)
{
    std::unique_lock lock(mutex_);

    auto vr = require_user_with_perm(connection_id, &UserPermissions::can_ban_user);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [admin, _] = std::get<ValidatedUser>(vr);

    auto gr = require_group(group_id);
    if (auto* err = std::get_if<RoomEvent>(&gr)) return *err;
    auto& group = std::get<GroupRef>(gr).it->second;

    if (!group.admins.count(admin->user_id) && admin->user_id != group.owner_id)
        return {error(ErrorCode::NotGroupAdmin)};

    auto target = std::string(target_user_id);
    group.members.erase(target);
    group.admins.erase(target);
    group.banned_users.insert(std::move(target));

    return {{
        {"type", "user_banned"},
        {"groupId", group_id},
        {"userId", target_user_id},
        {"bannedBy", admin->user_id},
    }, {}, true};
}

// ============================================================
// Query
// ============================================================

RoomEvent ChatRoom::get_online_users(std::string_view connection_id) const
{
    std::shared_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;

    nlohmann::json users = nlohmann::json::array();
    for (const auto& [id, u] : users_by_id_) {
        if (u->online) {
            users.push_back({
                {"userId", id},
                {"nickname", u->nickname},
                {"level", static_cast<int>(u->level)},
            });
        }
    }

    return {{{"type", "online_users"}, {"users", users}}};
}

RoomEvent ChatRoom::get_user_info(std::string_view connection_id, std::string_view target_user_id) const
{
    std::shared_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;

    auto uit = users_by_id_.find(target_user_id);
    if (uit == users_by_id_.end()) return {error(ErrorCode::UserNotFound)};

    const auto& target = uit->second;
    const auto perms = UserPermissions::for_level(target->level);

    return {{
        {"type", "user_info"},
        {"userId", target->user_id},
        {"username", target->username},
        {"nickname", target->nickname},
        {"level", static_cast<int>(target->level)},
        {"online", target->online.load()},
        {"lastActive", target->last_active},
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

RoomEvent ChatRoom::get_message_history(std::string_view connection_id, int limit, std::int64_t before_ts)
{
    std::shared_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;

    std::vector<MessageRecord> messages;
    // Fast path: ring buffer cache (no before_ts filter)
    if (before_ts == 0 && !recent_messages_.empty()) {
        std::size_t start = (recent_messages_.size() > static_cast<std::size_t>(limit))
                          ? recent_messages_.size() - limit : 0;
        messages.assign(recent_messages_.begin() + static_cast<long>(start), recent_messages_.end());
    } else if (before_ts > 0) {
        messages = db_->get_messages_before(before_ts, limit);
    } else {
        messages = db_->get_recent_messages(limit);
    }
    nlohmann::json history = nlohmann::json::array();
    for (const auto& msg : messages) history.push_back(message_to_json(msg));

    return {{{"type", "message_history"}, {"messages", history}}};
}

RoomEvent ChatRoom::get_private_message_history(std::string_view connection_id, std::string_view other_user_id, int limit)
{
    std::shared_lock lock(mutex_);

    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [user, _] = std::get<ValidatedUser>(vr);

    auto messages = db_->get_private_messages(user->user_id, std::string(other_user_id), limit);
    nlohmann::json history = nlohmann::json::array();
    for (const auto& msg : messages) {
        auto j = message_to_json(msg);
        j["receiverId"] = msg.receiver_id;
        history.push_back(std::move(j));
    }

    return {{{"type", "private_message_history"}, {"otherUserId", other_user_id}, {"messages", history}}};
}

// ── Search & Schedule (v2.3) ──────────────────────────────

RoomEvent ChatRoom::search_messages(std::string_view connection_id, std::string_view query, int limit)
{
    std::shared_lock lock(mutex_);
    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;

    auto results = db_->search_messages(query, limit);
    auto items = nlohmann::json::array();
    for (const auto& r : results) {
        items.push_back({
            {"messageId", r.message_id}, {"senderId", r.sender_id},
            {"senderNickname", r.sender_nickname}, {"text", r.text},
            {"chatId", r.receiver_id}, {"messageType", r.message_type},
            {"timestamp", r.timestamp}, {"rank", r.rank},
        });
    }
    nlohmann::json response{
        {"type", "search_results"},
        {"query", std::string(query)},
        {"results", std::move(items)},
        {"total", static_cast<int>(results.size())}
    };
    return RoomEvent{std::move(response)};
}

RoomEvent ChatRoom::schedule_message(std::string_view connection_id, std::string_view text,
                                      std::string_view chat_type, std::string_view chat_id, std::int64_t send_at)
{
    std::unique_lock lock(mutex_);
    auto vr = require_user(connection_id);
    if (auto* err = std::get_if<RoomEvent>(&vr)) return *err;
    auto& [user, _] = std::get<ValidatedUser>(vr);

    auto now = unix_seconds();
    if (send_at <= now) return {error(ErrorCode::InvalidFormat)};

    auto id = db_->schedule_message(user->user_id, chat_type, chat_id, text, send_at);
    if (id < 0) return {error(ErrorCode::InvalidFormat)};

    return {{{"type", "message_scheduled"}, {"scheduleId", id}, {"sendAt", send_at}}};
}

// ============================================================
// Error & lookup — constexpr O(1) tables (compiler-optimized)
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

nlohmann::json ChatRoom::error(ErrorCode code) const noexcept
{
    auto i = static_cast<std::size_t>(code);
    return {{"type", "error"}, {"code", kCodeNames[i]}, {"message", kCodeMessages[i]}};
}

std::string ChatRoom::connection_id_for_user(std::string_view user_id) const
{
    std::shared_lock lock(mutex_);
    auto it = users_by_id_.find(user_id);
    return (it != users_by_id_.end()) ? it->second->connection_id : std::string{};
}

const char* ChatRoom::code_name(ErrorCode code) noexcept { return kCodeNames[static_cast<std::size_t>(code)].data(); }
const char* ChatRoom::code_message(ErrorCode code) noexcept { return kCodeMessages[static_cast<std::size_t>(code)].data(); }

} // namespace imchat
