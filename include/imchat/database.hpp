#pragma once

#include "imchat/lru_cache.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace imchat {

struct UserRecord {
    std::string user_id;
    std::string username;
    std::string password_hash;
    std::string nickname;
    std::string avatar_url;
    std::int64_t created_at{0};
};

// SoA-friendly layout: hot fields (frequently accessed together) grouped first
// Cold fields (rarely accessed) at the end
struct MessageRecord {
    // Hot fields - accessed together for display
    std::int64_t id{0};
    std::int64_t timestamp{0};
    std::string sender_id;
    std::string sender_nickname;
    std::string text;

    // Warm fields - accessed for specific operations
    std::string receiver_id;
    std::string message_type;
    bool        is_recalled{false};

    // Cold fields - rarely accessed
    std::int64_t recalled_at{0};
    std::int64_t file_size{0};
    std::string file_url;
    std::string file_name;
    std::string read_by;      // JSON array of user_ids
};

// SoA container for batch message processing
struct MessageBatch {
    std::vector<std::int64_t> ids;
    std::vector<std::int64_t> timestamps;
    std::vector<std::string> sender_ids;
    std::vector<std::string> sender_nicknames;
    std::vector<std::string> texts;
    std::vector<std::string> receiver_ids;
    std::vector<std::string> message_types;
    std::vector<bool> is_recalled;

    void reserve(std::size_t n) {
        ids.reserve(n);
        timestamps.reserve(n);
        sender_ids.reserve(n);
        sender_nicknames.reserve(n);
        texts.reserve(n);
        receiver_ids.reserve(n);
        message_types.reserve(n);
        is_recalled.reserve(n);
    }

    void push_back(const MessageRecord& msg) {
        ids.push_back(msg.id);
        timestamps.push_back(msg.timestamp);
        sender_ids.push_back(msg.sender_id);
        sender_nicknames.push_back(msg.sender_nickname);
        texts.push_back(msg.text);
        receiver_ids.push_back(msg.receiver_id);
        message_types.push_back(msg.message_type);
        is_recalled.push_back(msg.is_recalled);
    }

    [[nodiscard]] std::size_t size() const noexcept { return ids.size(); }
};

// RAII wrapper for sqlite3_stmt
class SqliteStmt final {
public:
    SqliteStmt() = default;
    SqliteStmt(sqlite3* db, const char* sql, bool persistent = false);
    ~SqliteStmt();

    SqliteStmt(const SqliteStmt&) = delete;
    SqliteStmt& operator=(const SqliteStmt&) = delete;
    SqliteStmt(SqliteStmt&& other) noexcept;
    SqliteStmt& operator=(SqliteStmt&& other) noexcept;

    explicit operator bool() const noexcept { return stmt_ != nullptr; }
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return stmt_; }

    bool bind_text(int idx, const std::string& value) noexcept;
    bool bind_int64(int idx, std::int64_t value) noexcept;
    bool bind_int(int idx, int value) noexcept;

    // Step and return SQLITE_ROW / SQLITE_DONE / error code
    int step() noexcept;

    // Column accessors
    [[nodiscard]] std::string column_text(int idx) const noexcept;
    [[nodiscard]] std::int64_t column_int64(int idx) const noexcept;

private:
    sqlite3_stmt* stmt_{nullptr};
};

class Database final {
public:
    Database();
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool open(const std::string& db_path);
    void close();

    [[nodiscard]] bool create_user(const std::string& username, const std::string& password, const std::string& nickname);
    [[nodiscard]] std::optional<UserRecord> authenticate_user(const std::string& username, const std::string& password);
    [[nodiscard]] std::optional<UserRecord> find_user_by_id(std::string_view user_id);
    [[nodiscard]] std::optional<UserRecord> find_user_by_username(std::string_view username);
    [[nodiscard]] bool update_user_nickname(std::string_view user_id, std::string_view nickname);
    [[nodiscard]] bool update_user_avatar(std::string_view user_id, std::string_view avatar_url);

    // Session token — like HTTP cookie for fast re-auth (v2.2: device management)
    struct SessionInfo {
        std::string token;
        std::string user_id;
        std::string device_id;
        std::string device_name;
        std::int64_t created_at{0};
        std::int64_t expires_at{0};
    };
    [[nodiscard]] bool set_session_token(std::string_view user_id, std::string_view token,
                                         std::string_view device_id = "", std::string_view device_name = "");
    [[nodiscard]] std::optional<UserRecord> find_user_by_token(std::string_view token);
    [[nodiscard]] std::optional<SessionInfo> find_session_by_token(std::string_view token);
    [[nodiscard]] bool renew_session_token(std::string_view token);
    [[nodiscard]] std::vector<SessionInfo> get_user_sessions(std::string_view user_id);
    [[nodiscard]] bool remove_device_session(std::string_view token);
    [[nodiscard]] bool clear_session_token(std::string_view user_id);

    // Read receipt tracking (v2.2)
    [[nodiscard]] bool record_message_read(std::int64_t message_id, std::string_view user_id);
    [[nodiscard]] std::vector<std::string> get_message_readers(std::int64_t message_id);

    // FTS5 full-text search (v2.3)
    struct SearchResult {
        std::int64_t message_id;
        std::string sender_id;
        std::string sender_nickname;
        std::string text;
        std::string receiver_id;
        std::string message_type;
        std::int64_t timestamp;
        double rank;
    };
    [[nodiscard]] std::vector<SearchResult> search_messages(std::string_view query, int limit = 50, int offset = 0);

    // Scheduled messages (v2.3)
    [[nodiscard]] std::int64_t schedule_message(std::string_view sender_id, std::string_view chat_type,
                                                  std::string_view chat_id, std::string_view text, std::int64_t send_at);
    [[nodiscard]] std::vector<MessageRecord> get_due_scheduled_messages(std::int64_t before_ts);
    [[nodiscard]] bool mark_scheduled_sent(std::int64_t schedule_id);
    [[nodiscard]] bool save_message(std::string_view sender_id, std::string_view sender_nickname,
                      std::string_view receiver_id, std::string_view text,
                      std::string_view message_type = "text",
                      std::string_view file_url = "", std::string_view file_name = "",
                      std::int64_t file_size = 0);
    [[nodiscard]] std::vector<MessageRecord> get_recent_messages(int limit = 50);
    [[nodiscard]] std::vector<MessageRecord> get_messages_before(std::int64_t before_ts, int limit = 50);
    [[nodiscard]] std::vector<MessageRecord> get_private_messages(std::string_view user1_id, std::string_view user2_id, int limit = 50);
    [[nodiscard]] std::vector<MessageRecord> get_group_messages(std::string_view group_id, int limit = 50);
    [[nodiscard]] bool recall_message(std::int64_t message_id, std::string_view user_id);

    // Edit & Delete
    [[nodiscard]] bool edit_message(std::int64_t message_id, std::string_view user_id, std::string_view new_text);
    [[nodiscard]] bool delete_message(std::int64_t message_id, std::string_view user_id);

    // Pin messages
    [[nodiscard]] bool pin_message(std::int64_t message_id, std::string_view chat_id, std::string_view user_id);
    [[nodiscard]] bool unpin_message(std::int64_t message_id, std::string_view chat_id);
    [[nodiscard]] bool unpin_chat_messages(std::string_view chat_id);
    struct PinnedRecord {
        std::int64_t message_id;
        std::int64_t pinned_at;
        std::string chat_id;
        std::string pinned_by;
    };
    [[nodiscard]] std::vector<PinnedRecord> get_pinned_messages(std::string_view chat_id);

    // Saved messages
    [[nodiscard]] bool save_message_to_collection(std::int64_t message_id, std::string_view chat_id, std::string_view user_id);
    [[nodiscard]] bool unsave_message_from_collection(std::int64_t message_id, std::string_view user_id);
    struct SavedRecord {
        std::int64_t message_id;
        std::int64_t saved_at;
        std::string chat_id;
        std::string user_id;
    };
    [[nodiscard]] std::vector<SavedRecord> get_saved_messages(std::string_view user_id);

    // Direct message lookup (replaces O(n) scan in chat_room)
    [[nodiscard]] std::optional<MessageRecord> find_message_by_id(std::int64_t message_id);

    // Group persistence
    [[nodiscard]] bool create_group(std::string_view group_id, std::string_view group_name, std::string_view owner_id);
    [[nodiscard]] bool delete_group(std::string_view group_id);
    [[nodiscard]] bool add_group_member(std::string_view group_id, std::string_view user_id);
    [[nodiscard]] bool remove_group_member(std::string_view group_id, std::string_view user_id);
    [[nodiscard]] bool update_group_member_role(std::string_view group_id, std::string_view user_id, std::string_view role);
    [[nodiscard]] std::vector<std::tuple<std::string, std::string>> get_group_members(std::string_view group_id);
    [[nodiscard]] std::optional<std::tuple<std::string, std::string, std::string>> get_group_info(std::string_view group_id);
    [[nodiscard]] std::vector<std::tuple<std::string, std::string, std::string>> get_all_groups();

    // Read receipts
    [[nodiscard]] bool mark_chat_read(std::string_view chat_id, std::string_view user_id, std::int64_t last_read_id);
    [[nodiscard]] std::int64_t get_read_cursor(std::string_view chat_id, std::string_view user_id);
    [[nodiscard]] int get_unread_count(std::string_view chat_id, std::string_view user_id, std::int64_t since);

    // Batch message insert
    [[nodiscard]] bool save_messages_batch(const std::vector<MessageRecord>& messages);

    void clear_cache();

private:
    struct SchemaVersion { static constexpr int current = 7; };

    bool create_tables();
    bool run_migrations();

    [[nodiscard]] static std::int64_t current_timestamp() noexcept;
    [[nodiscard]] static std::string hash_password(const std::string& password);
    [[nodiscard]] static bool verify_password(const std::string& password, const std::string& hash) noexcept;

    [[nodiscard]] std::optional<UserRecord> fetch_user_by_column(const char* column, const std::string& value);
    [[nodiscard]] MessageRecord read_message_row(const SqliteStmt& stmt) const noexcept;

    sqlite3* db_{nullptr};

    LRUCache<std::string, UserRecord>          user_cache_;
    LRUCache<std::string, std::vector<MessageRecord>> message_cache_;

    mutable std::mutex db_mutex_;
};

} // namespace imchat
