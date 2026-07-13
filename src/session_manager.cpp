#include "imchat/session_manager.hpp"
#include "imchat/websocket_session.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <string_view>

namespace imchat {

// Compile-time string hash for dispatch optimization
[[nodiscard]] static constexpr std::uint64_t hash_string(std::string_view sv) noexcept {
    std::uint64_t hash = 14695981039346656037ULL; // FNV-1a offset basis
    for (char c : sv) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ULL; // FNV-1a prime
    }
    return hash;
}

// Helper for dispatch
[[nodiscard]] static constexpr std::uint64_t operator""_hash(const char* str, std::size_t len) noexcept {
    return hash_string(std::string_view{str, len});
}

// Precomputed FNV-1a hashes for dispatch — computed at compile-time
namespace dispatch_hash {
    // Auth
    constexpr auto hash_register       = hash_string("register");
    constexpr auto hash_login          = hash_string("login");
    constexpr auto hash_resume_session = hash_string("resume_session");
    constexpr auto hash_logout         = hash_string("logout");
    // Room
    constexpr auto hash_join           = hash_string("join");
    constexpr auto hash_send           = hash_string("send");
    constexpr auto hash_private_msg    = hash_string("private_message");
    constexpr auto hash_typing         = hash_string("typing");
    constexpr auto hash_recall         = hash_string("recall_message");
    constexpr auto hash_mark_read      = hash_string("mark_read");
    constexpr auto hash_send_file      = hash_string("send_file");
    constexpr auto hash_edit_message   = hash_string("edit_message");
    constexpr auto hash_delete_message = hash_string("delete_message");
    // Pin / Save
    constexpr auto hash_pin            = hash_string("pin_message");
    constexpr auto hash_unpin          = hash_string("unpin_message");
    constexpr auto hash_get_pinned     = hash_string("get_pinned_messages");
    constexpr auto hash_save           = hash_string("save_message");
    constexpr auto hash_unsave         = hash_string("unsave_message");
    constexpr auto hash_get_saved      = hash_string("get_saved_messages");
    // Group
    constexpr auto hash_create_group   = hash_string("create_group");
    constexpr auto hash_join_group     = hash_string("join_group");
    constexpr auto hash_leave_group    = hash_string("leave_group");
    constexpr auto hash_send_group     = hash_string("send_group_message");
    constexpr auto hash_get_members    = hash_string("get_group_members");
    constexpr auto hash_group_history  = hash_string("get_group_message_history");
    constexpr auto hash_kick           = hash_string("kick_user");
    constexpr auto hash_ban            = hash_string("ban_user");
    // Query
    constexpr auto hash_online_users   = hash_string("get_online_users");
    constexpr auto hash_user_info      = hash_string("get_user_info");
    constexpr auto hash_msg_history    = hash_string("get_message_history");
    constexpr auto hash_private_hist   = hash_string("get_private_message_history");
    constexpr auto hash_get_devices   = hash_string("get_devices");
    constexpr auto hash_search_msgs   = hash_string("search_messages");
    constexpr auto hash_schedule_msg  = hash_string("schedule_message");
}

SessionManager::SessionManager(const AppConfig& config, std::shared_ptr<Database> db)
    : config_(config)
    , room_(config.message_max_length, std::move(db))
{
}

// --- Core dispatch helpers ---

void SessionManager::dispatch_event(const SessionPtr& session, RoomEvent&& event)
{
    if (event.broadcast) {
        const auto text = event.payload.dump();
        if (event.target_user_id.empty()) {
            broadcast(text);
        } else {
            session->send(text);
            send_to_user(event.target_user_id, text);
        }
    } else {
        session->send(event.payload.dump());
    }
}

void SessionManager::dispatch_event(const SessionPtr& session, const RoomEvent& event)
{
    if (event.broadcast) {
        const auto text = event.payload.dump();
        if (event.target_user_id.empty()) {
            broadcast(text);
        } else {
            session->send(text);
            send_to_user(event.target_user_id, text);
        }
    } else {
        session->send(event.payload.dump());
    }
}

// --- Connection management ---

bool SessionManager::add(const SessionPtr& session)
{
    std::unique_lock lock(sessions_mutex_);
    if (sessions_.size() >= config_.max_connections) {
        session->send(room_.error(ErrorCode::RateLimited).dump());
        return false;
    }
    sessions_.insert(session);
    connection_map_.emplace(session->connection_id(), session);
    spdlog::info("session connected: {}", session->connection_id());
    return true;
}

void SessionManager::remove(const std::string& connection_id)
{
    auto leave_event = room_.leave(connection_id);

    {
        std::unique_lock lock(sessions_mutex_);
        connection_map_.erase(connection_id);
        std::erase_if(sessions_, [&](const auto& session) {
            return session->connection_id() == connection_id;
        });
    }

    if (leave_event.has_value() && leave_event->broadcast) {
        broadcast(leave_event->payload.dump());
    }

    spdlog::info("session disconnected: {}", connection_id);
}

void SessionManager::handle_message(const SessionPtr& session, std::string_view text)
{
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(text);
    } catch (const std::exception&) {
        send_error(session, ErrorCode::InvalidFormat);
        return;
    }

    using namespace dispatch_hash;
    const auto hash = hash_string(json.value("type", ""));

    // Compiler can optimize to jump table — all case labels are constexpr
    switch (hash) {
    // Auth
    case hash_register:       handle_register(session, json);        break;
    case hash_login:          handle_login(session, json);           break;
    case hash_resume_session: handle_resume_session(session, json);  break;
    case hash_logout:         handle_logout(session, json);          break;

    // Room — "send" is the hottest path
    case hash_join:           handle_join(session, json);            break;
    case hash_send:           [[likely]] handle_send(session, json); break;
    case hash_private_msg:    handle_private_message(session, json); break;
    case hash_typing:         handle_typing(session, json);          break;
    case hash_recall:         handle_recall_message(session, json);  break;
    case hash_mark_read:      handle_mark_read(session, json);       break;
    case hash_send_file:      handle_send_file(session, json);       break;
    case hash_edit_message:   handle_edit_message(session, json);    break;
    case hash_delete_message: handle_delete_message(session, json);  break;

    // Pin / Save
    case hash_pin:            handle_pin_message(session, json);      break;
    case hash_unpin:          handle_unpin_message(session, json);    break;
    case hash_get_pinned:     handle_get_pinned_messages(session, json); break;
    case hash_save:           handle_save_message(session, json);     break;
    case hash_unsave:         handle_unsave_message(session, json);   break;
    case hash_get_saved:      handle_get_saved_messages(session, json); break;

    // Group
    case hash_create_group:   handle_create_group(session, json);    break;
    case hash_join_group:     handle_join_group(session, json);      break;
    case hash_leave_group:    handle_leave_group(session, json);     break;
    case hash_send_group:     handle_send_group_message(session, json); break;
    case hash_get_members:    handle_get_group_members(session, json); break;
    case hash_group_history:  handle_get_group_message_history(session, json); break;
    case hash_kick:           handle_kick_user(session, json);       break;
    case hash_ban:            handle_ban_user(session, json);        break;

    // Query
    case hash_online_users:   handle_get_online_users(session, json);    break;
    case hash_user_info:      handle_get_user_info(session, json);       break;
    case hash_msg_history:    handle_get_message_history(session, json); break;
    case hash_private_hist:   handle_get_private_message_history(session, json); break;
    case hash_get_devices:   handle_get_devices(session, json); break;
    case hash_search_msgs:   handle_search_messages(session, json); break;
    case hash_schedule_msg:  handle_schedule_message(session, json); break;

    default: [[unlikely]] send_error(session, ErrorCode::InvalidFormat); break;
    }
}

// --- Auth handlers ---

void SessionManager::handle_register(const SessionPtr& session, const nlohmann::json& j)
{
    session->send(room_.register_user(
        session->connection_id(),
        j.value("username", ""),
        j.value("password", ""),
        j.value("nickname", ""),
        j.value("deviceName", "")
    ).payload.dump());
}

void SessionManager::handle_login(const SessionPtr& session, const nlohmann::json& j)
{
    session->send(room_.login(
        session->connection_id(),
        j.value("username", ""),
        j.value("password", ""),
        j.value("deviceName", "")
    ).payload.dump());
}

void SessionManager::handle_resume_session(const SessionPtr& session, const nlohmann::json& j)
{
    session->send(room_.resume_session(
        session->connection_id(),
        j.value("sessionToken", "")
    ).payload.dump());
}

void SessionManager::handle_logout(const SessionPtr& session, const nlohmann::json&)
{
    session->send(room_.logout(session->connection_id()).payload.dump());
}

// --- Room handlers ---

void SessionManager::handle_join(const SessionPtr& session, const nlohmann::json& j)
{
    auto result = room_.join(session->connection_id(), j.value("nickname", ""));
    session->send(result.payload.dump());

    if (result.ok) {
        auto users_event = room_.get_online_users(session->connection_id());
        if (users_event.payload.contains("users")) {
            broadcast(users_event.payload.dump());
        }
    }
}

void SessionManager::handle_send(const SessionPtr& session, const nlohmann::json& j)
{
    dispatch_event(session, room_.send_message(session->connection_id(), j.value("text", "")));
}

void SessionManager::handle_private_message(const SessionPtr& session, const nlohmann::json& j)
{
    auto receiver_id = j.value("receiverId", "");
    auto event = room_.send_private_message(session->connection_id(), receiver_id, j.value("text", ""));
    if (event.broadcast) {
        const auto text = event.payload.dump();
        session->send(text);
        send_to_user(receiver_id, text);
    } else {
        session->send(event.payload.dump());
    }
}

void SessionManager::handle_typing(const SessionPtr& session, const nlohmann::json&)
{
    const auto event = room_.typing(session->connection_id());
    if (event.broadcast) {
        broadcast(event.payload.dump(), session->connection_id());
    } else {
        session->send(event.payload.dump());
    }
}

void SessionManager::handle_recall_message(const SessionPtr& session, const nlohmann::json& j)
{
    dispatch_event(session, room_.recall_message(session->connection_id(), j.value("messageId", 0)));
}

void SessionManager::handle_mark_read(const SessionPtr& session, const nlohmann::json& j)
{
    auto event = room_.mark_read(session->connection_id(), j.value("messageId", 0));
    session->send(event.payload.dump());

    // v2.2: Broadcast read receipt to the message sender
    if (event.payload.contains("senderId") && !event.payload["senderId"].get<std::string>().empty()) {
        std::string sender_id = event.payload["senderId"];
        auto read_event = nlohmann::json{
            {"type", "message_read"},
            {"messageId", event.payload["messageId"]},
            {"readBy", event.payload["userId"]},
            {"readAt", event.payload["readAt"]},
        };
        send_to_user(sender_id, read_event.dump());
    }
}

void SessionManager::handle_send_file(const SessionPtr& session, const nlohmann::json& j)
{
    dispatch_event(session, room_.send_file_message(
        session->connection_id(),
        j.value("text", ""),
        j.value("fileUrl", ""),
        j.value("fileName", ""),
        j.value("fileSize", 0)
    ));
}

void SessionManager::handle_edit_message(const SessionPtr& session, const nlohmann::json& j)
{
    dispatch_event(session, room_.edit_message(
        session->connection_id(),
        j.value("messageId", 0),
        j.value("text", "")
    ));
}

void SessionManager::handle_delete_message(const SessionPtr& session, const nlohmann::json& j)
{
    dispatch_event(session, room_.delete_message(
        session->connection_id(),
        j.value("messageId", 0)
    ));
}

void SessionManager::handle_pin_message(const SessionPtr& session, const nlohmann::json& j)
{
    dispatch_event(session, room_.pin_message(
        session->connection_id(),
        j.value("messageId", 0),
        j.value("chatId", "")
    ));
}

void SessionManager::handle_unpin_message(const SessionPtr& session, const nlohmann::json& j)
{
    dispatch_event(session, room_.unpin_message(
        session->connection_id(),
        j.value("messageId", 0),
        j.value("chatId", "")
    ));
}

void SessionManager::handle_get_pinned_messages(const SessionPtr& session, const nlohmann::json& j)
{
    session->send(room_.get_pinned_messages(
        session->connection_id(),
        j.value("chatId", "")
    ).payload.dump());
}

void SessionManager::handle_save_message(const SessionPtr& session, const nlohmann::json& j)
{
    session->send(room_.save_message_to_collection(
        session->connection_id(),
        j.value("messageId", 0),
        j.value("chatId", "")
    ).payload.dump());
}

void SessionManager::handle_unsave_message(const SessionPtr& session, const nlohmann::json& j)
{
    session->send(room_.unsave_message_from_collection(
        session->connection_id(),
        j.value("messageId", 0)
    ).payload.dump());
}

void SessionManager::handle_get_saved_messages(const SessionPtr& session, const nlohmann::json&)
{
    session->send(room_.get_saved_messages(
        session->connection_id()
    ).payload.dump());
}

// --- Group handlers ---

void SessionManager::handle_create_group(const SessionPtr& session, const nlohmann::json& j)
{
    session->send(room_.create_group(session->connection_id(), j.value("name", "")).payload.dump());
}

void SessionManager::handle_join_group(const SessionPtr& session, const nlohmann::json& j)
{
    dispatch_event(session, room_.join_group(session->connection_id(), j.value("groupId", "")));
}

void SessionManager::handle_leave_group(const SessionPtr& session, const nlohmann::json& j)
{
    dispatch_event(session, room_.leave_group(session->connection_id(), j.value("groupId", "")));
}

void SessionManager::handle_send_group_message(const SessionPtr& session, const nlohmann::json& j)
{
    dispatch_event(session, room_.send_group_message(
        session->connection_id(), j.value("groupId", ""), j.value("text", "")
    ));
}

void SessionManager::handle_get_group_members(const SessionPtr& session, const nlohmann::json& j)
{
    session->send(room_.get_group_members(session->connection_id(), j.value("groupId", "")).payload.dump());
}

void SessionManager::handle_get_group_message_history(const SessionPtr& session, const nlohmann::json& j)
{
    session->send(room_.get_group_message_history(
        session->connection_id(), j.value("groupId", ""), j.value("limit", 50)
    ).payload.dump());
}

void SessionManager::handle_kick_user(const SessionPtr& session, const nlohmann::json& j)
{
    dispatch_event(session, room_.kick_user(
        session->connection_id(), j.value("targetUserId", ""), j.value("groupId", "")
    ));
}

void SessionManager::handle_ban_user(const SessionPtr& session, const nlohmann::json& j)
{
    dispatch_event(session, room_.ban_user(
        session->connection_id(), j.value("targetUserId", ""), j.value("groupId", "")
    ));
}

// --- Query handlers ---

void SessionManager::handle_get_online_users(const SessionPtr& session, const nlohmann::json&)
{
    session->send(room_.get_online_users(session->connection_id()).payload.dump());
}

void SessionManager::handle_get_user_info(const SessionPtr& session, const nlohmann::json& j)
{
    session->send(room_.get_user_info(session->connection_id(), j.value("targetUserId", "")).payload.dump());
}

void SessionManager::handle_get_message_history(const SessionPtr& session, const nlohmann::json& j)
{
    session->send(room_.get_message_history(
        session->connection_id(),
        j.value("limit", 50),
        j.value("before", 0)
    ).payload.dump());
}

void SessionManager::handle_get_private_message_history(const SessionPtr& session, const nlohmann::json& j)
{
    session->send(room_.get_private_message_history(
        session->connection_id(), j.value("otherUserId", ""), j.value("limit", 50)
    ).payload.dump());
}

// --- Utilities ---

void SessionManager::send_to_user(const std::string& user_id, const std::string& text)
{
    auto conn_id = room_.connection_id_for_user(user_id);
    if (conn_id.empty()) return;

    std::shared_lock lock(sessions_mutex_);
    auto it = connection_map_.find(conn_id);
    if (it != connection_map_.end()) {
        it->second->send(text);
    }
}

void SessionManager::broadcast(const std::string& text, const std::string& exclude_connection_id)
{
    std::shared_lock lock(sessions_mutex_);
    for (const auto& session : sessions_) {
        if (session->connection_id() != exclude_connection_id) {
            session->send(text);
        }
    }
}

void SessionManager::send_error(const SessionPtr& session, ErrorCode code)
{
    session->send(room_.error(code).dump());
}

// v2.2: Device management
void SessionManager::handle_get_devices(const SessionPtr& session, const nlohmann::json&)
{
    session->send(room_.get_user_devices(session->connection_id()).payload.dump());
}

// v2.3: Full-text search & scheduled messages
void SessionManager::handle_search_messages(const SessionPtr& session, const nlohmann::json& j)
{
    session->send(room_.search_messages(
        session->connection_id(),
        j.value("query", ""),
        j.value("limit", 50)
    ).payload.dump());
}

void SessionManager::handle_schedule_message(const SessionPtr& session, const nlohmann::json& j)
{
    session->send(room_.schedule_message(
        session->connection_id(),
        j.value("text", ""),
        j.value("chatType", "public"),
        j.value("chatId", ""),
        j.value("sendAt", 0)
    ).payload.dump());
}

} // namespace imchat
