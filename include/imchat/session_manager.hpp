#pragma once

#include "imchat/chat_room.hpp"
#include "imchat/config.hpp"
#include "imchat/database.hpp"

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace imchat {

class WebSocketSession;

class SessionManager final {
public:
    explicit SessionManager(const AppConfig& config, std::shared_ptr<Database> db);

    bool add(const std::shared_ptr<WebSocketSession>& session);
    void remove(const std::string& connection_id);
    void handle_message(const std::shared_ptr<WebSocketSession>& session, std::string_view text);
    void broadcast(const std::string& text, const std::string& exclude_connection_id = "");

private:
    using SessionPtr = std::shared_ptr<WebSocketSession>;

    // Core dispatch: send event to session, or broadcast if event.broadcast is set
    void dispatch_event(const SessionPtr& session, RoomEvent&& event);
    void dispatch_event(const SessionPtr& session, const RoomEvent& event);

    // Send error to session
    void send_error(const SessionPtr& session, ErrorCode code);

    // Find and send to a specific user
    void send_to_user(const std::string& user_id, const std::string& text);

    // --- Auth handlers ---
    void handle_register(const SessionPtr& session, const nlohmann::json& json);
    void handle_login(const SessionPtr& session, const nlohmann::json& json);
    void handle_resume_session(const SessionPtr& session, const nlohmann::json& json);
    void handle_logout(const SessionPtr& session, const nlohmann::json& json);

    // --- Room handlers ---
    void handle_join(const SessionPtr& session, const nlohmann::json& json);
    void handle_send(const SessionPtr& session, const nlohmann::json& json);
    void handle_private_message(const SessionPtr& session, const nlohmann::json& json);
    void handle_typing(const SessionPtr& session, const nlohmann::json& json);
    void handle_recall_message(const SessionPtr& session, const nlohmann::json& json);
    void handle_mark_read(const SessionPtr& session, const nlohmann::json& json);
    void handle_send_file(const SessionPtr& session, const nlohmann::json& json);
    void handle_edit_message(const SessionPtr& session, const nlohmann::json& json);
    void handle_delete_message(const SessionPtr& session, const nlohmann::json& json);

    // --- Pin & Save handlers ---
    void handle_pin_message(const SessionPtr& session, const nlohmann::json& json);
    void handle_unpin_message(const SessionPtr& session, const nlohmann::json& json);
    void handle_get_pinned_messages(const SessionPtr& session, const nlohmann::json& json);
    void handle_save_message(const SessionPtr& session, const nlohmann::json& json);
    void handle_unsave_message(const SessionPtr& session, const nlohmann::json& json);
    void handle_get_saved_messages(const SessionPtr& session, const nlohmann::json& json);

    // --- Group handlers ---
    void handle_create_group(const SessionPtr& session, const nlohmann::json& json);
    void handle_join_group(const SessionPtr& session, const nlohmann::json& json);
    void handle_leave_group(const SessionPtr& session, const nlohmann::json& json);
    void handle_send_group_message(const SessionPtr& session, const nlohmann::json& json);
    void handle_get_group_members(const SessionPtr& session, const nlohmann::json& json);
    void handle_get_group_message_history(const SessionPtr& session, const nlohmann::json& json);
    void handle_kick_user(const SessionPtr& session, const nlohmann::json& json);
    void handle_ban_user(const SessionPtr& session, const nlohmann::json& json);

    // --- Query handlers ---
    void handle_get_online_users(const SessionPtr& session, const nlohmann::json& json);
    void handle_get_user_info(const SessionPtr& session, const nlohmann::json& json);
    void handle_get_message_history(const SessionPtr& session, const nlohmann::json& json);
    void handle_get_private_message_history(const SessionPtr& session, const nlohmann::json& json);
    void handle_get_devices(const SessionPtr& session, const nlohmann::json& json);
    void handle_search_messages(const SessionPtr& session, const nlohmann::json& json);
    void handle_schedule_message(const SessionPtr& session, const nlohmann::json& json);

    AppConfig config_;
    ChatRoom room_;
    mutable std::shared_mutex sessions_mutex_;
    std::unordered_set<std::shared_ptr<WebSocketSession>> sessions_;
    std::unordered_map<std::string, std::shared_ptr<WebSocketSession>> connection_map_;
};

} // namespace imchat
