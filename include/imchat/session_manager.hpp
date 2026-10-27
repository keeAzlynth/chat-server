#pragma once

#include "imchat/chat_room.hpp"
#include "imchat/config.hpp"
#include "imchat/database.hpp"
#include "imchat/thread_pool.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace imchat {

class WebSocketSession;

class SessionManager final {
public:
    explicit SessionManager(const AppConfig& config, std::shared_ptr<Database> db);
    ~SessionManager();

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    bool add(const std::shared_ptr<WebSocketSession>& session);
    void remove(const std::string& connection_id);

    // Called on an I/O thread: parses the frame, then hands the command to the
    // worker shard that owns this connection so it runs off the reactor and
    // in submission order.
    void handle_message(const std::shared_ptr<WebSocketSession>& session, std::string_view text);

    void broadcast(const std::string& text, const std::string& exclude_connection_id = "");

    // Stops accepting work and waits for the queue to drain.
    void stop();

    struct Metrics {
        std::uint64_t connections{0};
        std::uint64_t commands{0};
        std::uint64_t messages_in{0};
        std::uint64_t bytes_in{0};
        std::uint64_t messages_out{0};
        std::uint64_t bytes_out{0};
        std::uint64_t rejected{0};
        std::size_t   queued{0};
    };
    [[nodiscard]] Metrics metrics() const;
    [[nodiscard]] ChatRoom::Stats room_stats() const { return room_.stats(); }

private:
    using SessionPtr = std::shared_ptr<WebSocketSession>;

    void handle_command(const SessionPtr& session, const nlohmann::json& json);
    void dispatch(const SessionPtr& session, RoomEvent&& event);
    void send_to_user(const std::string& user_id, const std::string& text);
    void send_error(const SessionPtr& session, ErrorCode code);
    void send_online_users(const SessionPtr& session);

    // ── Auth ─────────────────────────────────────────────
    void handle_register(const SessionPtr&, const nlohmann::json&);
    void handle_login(const SessionPtr&, const nlohmann::json&);
    void handle_resume_session(const SessionPtr&, const nlohmann::json&);
    void handle_logout(const SessionPtr&, const nlohmann::json&);
    void handle_get_devices(const SessionPtr&, const nlohmann::json&);

    // ── Room ─────────────────────────────────────────────
    void handle_join(const SessionPtr&, const nlohmann::json&);
    void handle_send(const SessionPtr&, const nlohmann::json&);
    void handle_private_message(const SessionPtr&, const nlohmann::json&);
    void handle_typing(const SessionPtr&, const nlohmann::json&);
    void handle_recall_message(const SessionPtr&, const nlohmann::json&);
    void handle_mark_read(const SessionPtr&, const nlohmann::json&);
    void handle_send_file(const SessionPtr&, const nlohmann::json&);
    void handle_edit_message(const SessionPtr&, const nlohmann::json&);
    void handle_delete_message(const SessionPtr&, const nlohmann::json&);

    // ── Pin & save ───────────────────────────────────────
    void handle_pin_message(const SessionPtr&, const nlohmann::json&);
    void handle_unpin_message(const SessionPtr&, const nlohmann::json&);
    void handle_get_pinned_messages(const SessionPtr&, const nlohmann::json&);
    void handle_save_message(const SessionPtr&, const nlohmann::json&);
    void handle_unsave_message(const SessionPtr&, const nlohmann::json&);
    void handle_get_saved_messages(const SessionPtr&, const nlohmann::json&);

    // ── Group ────────────────────────────────────────────
    void handle_create_group(const SessionPtr&, const nlohmann::json&);
    void handle_join_group(const SessionPtr&, const nlohmann::json&);
    void handle_leave_group(const SessionPtr&, const nlohmann::json&);
    void handle_send_group_message(const SessionPtr&, const nlohmann::json&);
    void handle_get_group_members(const SessionPtr&, const nlohmann::json&);
    void handle_get_group_message_history(const SessionPtr&, const nlohmann::json&);
    void handle_list_groups(const SessionPtr&, const nlohmann::json&);
    void handle_kick_user(const SessionPtr&, const nlohmann::json&);
    void handle_ban_user(const SessionPtr&, const nlohmann::json&);

    // ── Query ────────────────────────────────────────────
    void handle_get_online_users(const SessionPtr&, const nlohmann::json&);
    void handle_get_user_info(const SessionPtr&, const nlohmann::json&);
    void handle_get_message_history(const SessionPtr&, const nlohmann::json&);
    void handle_get_private_message_history(const SessionPtr&, const nlohmann::json&);
    void handle_search_messages(const SessionPtr&, const nlohmann::json&);
    void handle_schedule_message(const SessionPtr&, const nlohmann::json&);

    AppConfig    config_;
    ChatRoom     room_;
    ShardedExecutor executor_;

    mutable std::shared_mutex sessions_mutex_;
    std::unordered_map<std::string, SessionPtr> sessions_;

    mutable std::atomic<std::uint64_t> commands_{0};
    mutable std::atomic<std::uint64_t> messages_in_{0};
    mutable std::atomic<std::uint64_t> bytes_in_{0};
    mutable std::atomic<std::uint64_t> messages_out_{0};
    mutable std::atomic<std::uint64_t> bytes_out_{0};
    mutable std::atomic<std::uint64_t> rejected_{0};
};

} // namespace imchat
