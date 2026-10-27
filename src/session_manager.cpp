#include "imchat/session_manager.hpp"
#include "imchat/websocket_session.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <string_view>
#include <vector>

namespace imchat {
namespace {

// ── Compile-time dispatch ────────────────────────────────
// FNV-1a over the message type, evaluated at compile time so the switch below
// becomes a jump table instead of a chain of string compares.
[[nodiscard]] constexpr std::uint64_t hash_string(std::string_view sv) noexcept
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (char c : sv) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ULL;
    }
    return hash;
}

namespace dispatch_hash {
    // Auth
    constexpr auto register_user    = hash_string("register");
    constexpr auto login            = hash_string("login");
    constexpr auto resume_session   = hash_string("resume_session");
    constexpr auto logout           = hash_string("logout");
    constexpr auto get_devices      = hash_string("get_devices");
    // Room
    constexpr auto join             = hash_string("join");
    constexpr auto send             = hash_string("send");
    constexpr auto private_message  = hash_string("private_message");
    constexpr auto typing           = hash_string("typing");
    constexpr auto recall           = hash_string("recall_message");
    constexpr auto mark_read        = hash_string("mark_read");
    constexpr auto send_file        = hash_string("send_file");
    constexpr auto edit_message     = hash_string("edit_message");
    constexpr auto delete_message   = hash_string("delete_message");
    // Pin / save
    constexpr auto pin              = hash_string("pin_message");
    constexpr auto unpin            = hash_string("unpin_message");
    constexpr auto get_pinned       = hash_string("get_pinned_messages");
    constexpr auto save             = hash_string("save_message");
    constexpr auto unsave           = hash_string("unsave_message");
    constexpr auto get_saved        = hash_string("get_saved_messages");
    // Group
    constexpr auto create_group     = hash_string("create_group");
    constexpr auto join_group       = hash_string("join_group");
    constexpr auto leave_group      = hash_string("leave_group");
    constexpr auto send_group       = hash_string("send_group_message");
    constexpr auto send_group_alias = hash_string("group_message");
    constexpr auto get_members      = hash_string("get_group_members");
    constexpr auto group_history    = hash_string("get_group_message_history");
    constexpr auto list_groups      = hash_string("get_groups");
    constexpr auto kick             = hash_string("kick_user");
    constexpr auto ban              = hash_string("ban_user");
    // Query
    constexpr auto online_users     = hash_string("get_online_users");
    constexpr auto user_info        = hash_string("get_user_info");
    constexpr auto msg_history      = hash_string("get_message_history");
    constexpr auto private_history  = hash_string("get_private_message_history");
    constexpr auto search           = hash_string("search_messages");
    constexpr auto schedule         = hash_string("schedule_message");
    // Transport
    constexpr auto ping             = hash_string("ping");
}

// Clients are untrusted: a field of the wrong JSON type must not throw inside
// the handler (nlohmann's value<T>() does).
[[nodiscard]] std::string json_str(const nlohmann::json& json, const char* key)
{
    auto it = json.find(key);
    if (it == json.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

[[nodiscard]] std::int64_t json_i64(const nlohmann::json& json, const char* key,
                                    std::int64_t fallback = 0)
{
    auto it = json.find(key);
    if (it == json.end()) return fallback;
    if (it->is_number_integer()) return it->get<std::int64_t>();
    if (it->is_number_unsigned()) return static_cast<std::int64_t>(it->get<std::uint64_t>());
    if (it->is_number_float()) return static_cast<std::int64_t>(it->get<double>());
    if (it->is_string()) {
        try {
            return std::stoll(it->get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

[[nodiscard]] int json_int(const nlohmann::json& json, const char* key, int fallback = 0)
{
    return static_cast<int>(json_i64(json, key, fallback));
}

[[nodiscard]] std::int64_t now_seconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

// ── Lifecycle ────────────────────────────────────────────

SessionManager::SessionManager(const AppConfig& config, std::shared_ptr<Database> db)
    : config_(config)
    , room_(config, std::move(db))
    , executor_(config.worker_threads)
{
    spdlog::info("command workers: {}", executor_.size());
}

SessionManager::~SessionManager()
{
    stop();
}

void SessionManager::stop()
{
    executor_.stop(true);

    std::vector<SessionPtr> sessions;
    {
        std::unique_lock lock(sessions_mutex_);
        sessions.reserve(sessions_.size());
        for (auto& [id, session] : sessions_) sessions.push_back(session);
        sessions_.clear();
    }
    for (const auto& session : sessions) {
        if (session) session->close();
    }
}

// ── Connection registry ──────────────────────────────────

bool SessionManager::add(const SessionPtr& session)
{
    bool accepted = false;
    {
        std::unique_lock lock(sessions_mutex_);
        if (sessions_.size() >= config_.max_connections) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
        } else {
            sessions_.emplace(session->connection_id(), session);
            accepted = true;
        }
    }

    if (!accepted) {
        session->send(room_.error(ErrorCode::RateLimited).dump());
        return false;
    }

    spdlog::debug("session connected: {}", session->connection_id());
    return true;
}

void SessionManager::remove(const std::string& connection_id)
{
    {
        std::unique_lock lock(sessions_mutex_);
        sessions_.erase(connection_id);
    }

    room_.forget_connection(connection_id);

    // `leave` returns a broadcast only when the last device of a user went
    // away, so a phone disconnect no longer marks a browser session offline.
    if (auto event = room_.leave(connection_id)) {
        dispatch(nullptr, std::move(*event));
    }

    spdlog::debug("session disconnected: {}", connection_id);
}

// ── Inbound ──────────────────────────────────────────────

void SessionManager::handle_message(const SessionPtr& session, std::string_view text)
{
    messages_in_.fetch_add(1, std::memory_order_relaxed);
    bytes_in_.fetch_add(text.size(), std::memory_order_relaxed);

    nlohmann::json json;
    try {
        json = nlohmann::json::parse(text);
    } catch (const std::exception&) {
        send_error(session, ErrorCode::InvalidFormat);
        return;
    }

    if (!json.is_object()) {
        send_error(session, ErrorCode::InvalidFormat);
        return;
    }

    // Route by connection: same shard => same worker => FIFO per connection,
    // different connections still run in parallel.
    const auto key = session->connection_id();
    executor_.post(key, [this, session, json = std::move(json)]() mutable {
        handle_command(session, json);
    });
}

void SessionManager::handle_command(const SessionPtr& session, const nlohmann::json& json)
{
    commands_.fetch_add(1, std::memory_order_relaxed);

    if (session->is_closed()) return;

    using namespace dispatch_hash;
    const auto type = json_str(json, "type");
    if (type.empty()) {
        send_error(session, ErrorCode::InvalidFormat);
        return;
    }

    switch (hash_string(type)) {
    // Auth
    case register_user:    handle_register(session, json);        break;
    case login:            handle_login(session, json);           break;
    case resume_session:   handle_resume_session(session, json);  break;
    case logout:           handle_logout(session, json);          break;
    case get_devices:      handle_get_devices(session, json);     break;

    // Room — "send" is the hottest path
    case join:             handle_join(session, json);            break;
    case send:             handle_send(session, json);            break;
    case private_message:  handle_private_message(session, json); break;
    case typing:           handle_typing(session, json);          break;
    case recall:           handle_recall_message(session, json);  break;
    case mark_read:        handle_mark_read(session, json);       break;
    case send_file:        handle_send_file(session, json);       break;
    case edit_message:     handle_edit_message(session, json);    break;
    case delete_message:   handle_delete_message(session, json);  break;

    // Pin / save
    case pin:              handle_pin_message(session, json);     break;
    case unpin:            handle_unpin_message(session, json);   break;
    case get_pinned:       handle_get_pinned_messages(session, json); break;
    case save:             handle_save_message(session, json);    break;
    case unsave:           handle_unsave_message(session, json);  break;
    case get_saved:        handle_get_saved_messages(session, json); break;

    // Group
    case create_group:     handle_create_group(session, json);    break;
    case join_group:       handle_join_group(session, json);      break;
    case leave_group:      handle_leave_group(session, json);     break;
    case send_group:
    case send_group_alias: handle_send_group_message(session, json); break;
    case get_members:      handle_get_group_members(session, json); break;
    case group_history:    handle_get_group_message_history(session, json); break;
    case list_groups:      handle_list_groups(session, json);     break;
    case kick:             handle_kick_user(session, json);       break;
    case ban:              handle_ban_user(session, json);        break;

    // Query
    case online_users:     handle_get_online_users(session, json); break;
    case user_info:        handle_get_user_info(session, json);    break;
    case msg_history:      handle_get_message_history(session, json); break;
    case private_history:  handle_get_private_message_history(session, json); break;
    case search:           handle_search_messages(session, json); break;
    case schedule:         handle_schedule_message(session, json); break;

    case ping:
        session->send(nlohmann::json{
            {"type", "pong"},
            {"ts", json_i64(json, "ts")},
            {"serverTs", now_seconds()},
        }.dump());
        break;

    default:
        send_error(session, ErrorCode::InvalidFormat);
        break;
    }
}

// ── Outbound ─────────────────────────────────────────────

void SessionManager::dispatch(const SessionPtr& session, RoomEvent&& event)
{
    const auto text = event.payload.dump();
    messages_out_.fetch_add(1, std::memory_order_relaxed);

    const bool has_ack = event.ack && session;
    if (has_ack) session->send(text);

    if (event.broadcast) {
        broadcast(text, has_ack ? session->connection_id() : std::string{});
        return;
    }

    bytes_out_.fetch_add(text.size() * (1 + event.recipients.size()), std::memory_order_relaxed);
    for (const auto& user_id : event.recipients) send_to_user(user_id, text);
}

void SessionManager::send_to_user(const std::string& user_id, const std::string& text)
{
    const auto connections = room_.connections_for_user(user_id);
    if (connections.empty()) return;

    std::shared_lock lock(sessions_mutex_);
    for (const auto& connection_id : connections) {
        auto it = sessions_.find(connection_id);
        if (it != sessions_.end()) it->second->send(text);
    }
}

void SessionManager::broadcast(const std::string& text, const std::string& exclude_connection_id)
{
    // Snapshot under the lock, then send with the lock released: a slow client
    // must not block session registration.
    std::vector<SessionPtr> targets;
    {
        std::shared_lock lock(sessions_mutex_);
        targets.reserve(sessions_.size());
        for (const auto& [id, session] : sessions_) {
            if (exclude_connection_id.empty() || id != exclude_connection_id) {
                targets.push_back(session);
            }
        }
    }

    bytes_out_.fetch_add(text.size() * targets.size(), std::memory_order_relaxed);
    for (const auto& session : targets) session->send(text);
}

void SessionManager::send_error(const SessionPtr& session, ErrorCode code)
{
    if (session) session->send(room_.error(code).dump());
}

void SessionManager::send_online_users(const SessionPtr& session)
{
    auto event = room_.get_online_users(session->connection_id());
    if (event.payload.contains("users")) {
        broadcast(event.payload.dump());
    }
}

SessionManager::Metrics SessionManager::metrics() const
{
    Metrics metrics;
    {
        std::shared_lock lock(sessions_mutex_);
        metrics.connections = sessions_.size();
    }
    metrics.commands    = commands_.load(std::memory_order_relaxed);
    metrics.messages_in = messages_in_.load(std::memory_order_relaxed);
    metrics.bytes_in    = bytes_in_.load(std::memory_order_relaxed);
    metrics.messages_out = messages_out_.load(std::memory_order_relaxed);
    metrics.bytes_out   = bytes_out_.load(std::memory_order_relaxed);
    metrics.rejected    = rejected_.load(std::memory_order_relaxed);
    metrics.queued      = executor_.pending();
    return metrics;
}

// ── Auth handlers ────────────────────────────────────────

void SessionManager::handle_register(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.register_user(
        session->connection_id(),
        json_str(json, "username"),
        json_str(json, "password"),
        json_str(json, "nickname"),
        json_str(json, "deviceName")));
}

void SessionManager::handle_login(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.login(
        session->connection_id(),
        json_str(json, "username"),
        json_str(json, "password"),
        json_str(json, "deviceName")));
}

void SessionManager::handle_resume_session(const SessionPtr& session, const nlohmann::json& json)
{
    auto token = json_str(json, "sessionToken");
    if (token.empty()) token = json_str(json, "token");

    dispatch(session, room_.resume_session(session->connection_id(), token));
}

void SessionManager::handle_logout(const SessionPtr& session, const nlohmann::json&)
{
    dispatch(session, room_.logout(session->connection_id()));
}

void SessionManager::handle_get_devices(const SessionPtr& session, const nlohmann::json&)
{
    dispatch(session, room_.get_user_devices(session->connection_id()));
}

// ── Room handlers ────────────────────────────────────────

void SessionManager::handle_join(const SessionPtr& session, const nlohmann::json& json)
{
    auto result = room_.join(session->connection_id(), json_str(json, "nickname"));
    session->send(result.payload.dump());
    if (result.ok) send_online_users(session);
}

void SessionManager::handle_send(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.send_message(session->connection_id(), json_str(json, "text")));
}

void SessionManager::handle_private_message(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.send_private_message(
        session->connection_id(),
        json_str(json, "receiverId"),
        json_str(json, "text")));
}

void SessionManager::handle_typing(const SessionPtr& session, const nlohmann::json&)
{
    dispatch(session, room_.typing(session->connection_id()));
}

void SessionManager::handle_recall_message(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.recall_message(
        session->connection_id(), json_i64(json, "messageId")));
}

void SessionManager::handle_mark_read(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.mark_read(
        session->connection_id(), json_i64(json, "messageId")));
}

void SessionManager::handle_send_file(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.send_file_message(
        session->connection_id(),
        json_str(json, "text"),
        json_str(json, "fileUrl"),
        json_str(json, "fileName"),
        json_i64(json, "fileSize")));
}

void SessionManager::handle_edit_message(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.edit_message(
        session->connection_id(),
        json_i64(json, "messageId"),
        json_str(json, "text")));
}

void SessionManager::handle_delete_message(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.delete_message(
        session->connection_id(), json_i64(json, "messageId")));
}

// ── Pin / save handlers ──────────────────────────────────

void SessionManager::handle_pin_message(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.pin_message(
        session->connection_id(),
        json_i64(json, "messageId"),
        json_str(json, "chatId")));
}

void SessionManager::handle_unpin_message(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.unpin_message(
        session->connection_id(),
        json_i64(json, "messageId"),
        json_str(json, "chatId")));
}

void SessionManager::handle_get_pinned_messages(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.get_pinned_messages(
        session->connection_id(), json_str(json, "chatId")));
}

void SessionManager::handle_save_message(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.save_message_to_collection(
        session->connection_id(),
        json_i64(json, "messageId"),
        json_str(json, "chatId")));
}

void SessionManager::handle_unsave_message(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.unsave_message_from_collection(
        session->connection_id(), json_i64(json, "messageId")));
}

void SessionManager::handle_get_saved_messages(const SessionPtr& session, const nlohmann::json&)
{
    dispatch(session, room_.get_saved_messages(session->connection_id()));
}

// ── Group handlers ───────────────────────────────────────

void SessionManager::handle_create_group(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.create_group(
        session->connection_id(), json_str(json, "name")));
}

void SessionManager::handle_join_group(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.join_group(
        session->connection_id(), json_str(json, "groupId")));
}

void SessionManager::handle_leave_group(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.leave_group(
        session->connection_id(), json_str(json, "groupId")));
}

void SessionManager::handle_send_group_message(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.send_group_message(
        session->connection_id(),
        json_str(json, "groupId"),
        json_str(json, "text")));
}

void SessionManager::handle_get_group_members(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.get_group_members(
        session->connection_id(), json_str(json, "groupId")));
}

void SessionManager::handle_get_group_message_history(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.get_group_message_history(
        session->connection_id(),
        json_str(json, "groupId"),
        json_int(json, "limit", 50)));
}

void SessionManager::handle_list_groups(const SessionPtr& session, const nlohmann::json&)
{
    dispatch(session, room_.list_groups(session->connection_id()));
}

void SessionManager::handle_kick_user(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.kick_user(
        session->connection_id(),
        json_str(json, "targetUserId"),
        json_str(json, "groupId")));
}

void SessionManager::handle_ban_user(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.ban_user(
        session->connection_id(),
        json_str(json, "targetUserId"),
        json_str(json, "groupId")));
}

// ── Query handlers ───────────────────────────────────────

void SessionManager::handle_get_online_users(const SessionPtr& session, const nlohmann::json&)
{
    dispatch(session, room_.get_online_users(session->connection_id()));
}

void SessionManager::handle_get_user_info(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.get_user_info(
        session->connection_id(), json_str(json, "targetUserId")));
}

void SessionManager::handle_get_message_history(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.get_message_history(
        session->connection_id(),
        json_int(json, "limit", 50),
        json_i64(json, "before")));
}

void SessionManager::handle_get_private_message_history(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.get_private_message_history(
        session->connection_id(),
        json_str(json, "otherUserId"),
        json_int(json, "limit", 50)));
}

void SessionManager::handle_search_messages(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.search_messages(
        session->connection_id(),
        json_str(json, "query"),
        json_int(json, "limit", 50)));
}

void SessionManager::handle_schedule_message(const SessionPtr& session, const nlohmann::json& json)
{
    dispatch(session, room_.schedule_message(
        session->connection_id(),
        json_str(json, "text"),
        json_str(json, "chatType"),
        json_str(json, "chatId"),
        json_i64(json, "sendAt")));
}

} // namespace imchat
