#include "imchat/database.hpp"

#include <sqlite3.h>
#include <sodium.h>

#include <chrono>
#include <stdexcept>

namespace imchat {

// ============================================================
// SqliteStmt — RAII wrapper
// ============================================================

SqliteStmt::SqliteStmt(sqlite3* db, const char* sql, bool persistent)
{
    int flags = persistent ? SQLITE_PREPARE_PERSISTENT : 0;
    if (sqlite3_prepare_v3(db, sql, -1, flags, &stmt_, nullptr) != SQLITE_OK) {
        stmt_ = nullptr;
    }
}

SqliteStmt::~SqliteStmt()
{
    if (stmt_) sqlite3_finalize(stmt_);
}

SqliteStmt::SqliteStmt(SqliteStmt&& other) noexcept : stmt_(other.stmt_)
{
    other.stmt_ = nullptr;
}

SqliteStmt& SqliteStmt::operator=(SqliteStmt&& other) noexcept
{
    if (this != &other) {
        if (stmt_) sqlite3_finalize(stmt_);
        stmt_ = other.stmt_;
        other.stmt_ = nullptr;
    }
    return *this;
}

bool SqliteStmt::bind_text(int idx, const std::string& value) noexcept
{
    return sqlite3_bind_text(stmt_, idx, value.c_str(), -1, SQLITE_STATIC) == SQLITE_OK;
}

bool SqliteStmt::bind_int64(int idx, std::int64_t value) noexcept
{
    return sqlite3_bind_int64(stmt_, idx, value) == SQLITE_OK;
}

bool SqliteStmt::bind_int(int idx, int value) noexcept
{
    return sqlite3_bind_int(stmt_, idx, value) == SQLITE_OK;
}

int SqliteStmt::step() noexcept
{
    return sqlite3_step(stmt_);
}

std::string SqliteStmt::column_text(int idx) const noexcept
{
    auto* ptr = static_cast<const char*>(static_cast<const void*>(sqlite3_column_text(stmt_, idx)));
    return ptr ? ptr : "";
}

std::int64_t SqliteStmt::column_int64(int idx) const noexcept
{
    return sqlite3_column_int64(stmt_, idx);
}

// ============================================================
// Database
// ============================================================

Database::Database()
    : user_cache_(1000, std::chrono::seconds{300})
    , message_cache_(100, std::chrono::seconds{60})
{
    if (sodium_init() < 0) {
        spdlog::error("Failed to initialize libsodium");
    }
}

Database::~Database()
{
    close();
}

bool Database::open(const std::string& db_path)
{
    std::lock_guard lock(db_mutex_);

    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        spdlog::error("Failed to open database: {}", sqlite3_errmsg(db));
        sqlite3_close(db);
        return false;
    }

    sqlite3_exec(db, "PRAGMA journal_mode=WAL",   nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL",  nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA cache_size=-64000",   nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA temp_store=MEMORY",   nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA mmap_size=268435456", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA page_size=4096",      nullptr, nullptr, nullptr);

    db_ = db;
    return create_tables();
}

void Database::close()
{
    std::lock_guard lock(db_mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    user_cache_.clear();
    message_cache_.clear();
}

bool Database::create_tables()
{
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL);
        INSERT INTO schema_version (version) SELECT 0 WHERE NOT EXISTS (SELECT 1 FROM schema_version);

        CREATE TABLE IF NOT EXISTS users (
            user_id TEXT PRIMARY KEY,
            username TEXT UNIQUE NOT NULL,
            password_hash TEXT NOT NULL,
            nickname TEXT NOT NULL,
            avatar_url TEXT DEFAULT '',
            created_at INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS messages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            sender_id TEXT NOT NULL,
            sender_nickname TEXT NOT NULL,
            receiver_id TEXT DEFAULT '',
            text TEXT NOT NULL,
            message_type TEXT DEFAULT 'text',
            file_url TEXT DEFAULT '',
            file_name TEXT DEFAULT '',
            file_size INTEGER DEFAULT 0,
            is_recalled INTEGER DEFAULT 0,
            recalled_at INTEGER DEFAULT 0,
            read_by TEXT DEFAULT '[]',
            timestamp INTEGER NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_messages_timestamp ON messages(timestamp);
        CREATE INDEX IF NOT EXISTS idx_messages_receiver  ON messages(receiver_id);
        CREATE INDEX IF NOT EXISTS idx_messages_sender    ON messages(sender_id);
        CREATE INDEX IF NOT EXISTS idx_messages_receiver_type ON messages(receiver_id, message_type);
        CREATE INDEX IF NOT EXISTS idx_messages_group_time ON messages(receiver_id, timestamp);
        CREATE INDEX IF NOT EXISTS idx_users_username     ON users(username);

        CREATE TABLE IF NOT EXISTS groups_data (
            group_id TEXT PRIMARY KEY,
            group_name TEXT NOT NULL,
            owner_id TEXT NOT NULL,
            created_at INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS group_members (
            group_id TEXT NOT NULL,
            user_id TEXT NOT NULL,
            role TEXT DEFAULT 'member',
            PRIMARY KEY (group_id, user_id)
        );

        CREATE TABLE IF NOT EXISTS message_reads (
            message_id INTEGER NOT NULL,
            user_id TEXT NOT NULL,
            read_at INTEGER NOT NULL,
            PRIMARY KEY (message_id, user_id)
        );
        CREATE INDEX IF NOT EXISTS idx_message_reads_message ON message_reads(message_id);

        CREATE INDEX IF NOT EXISTS idx_messages_id ON messages(id);
    )";

    char* err_msg = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        spdlog::error("Failed to create tables: {}", err_msg);
        sqlite3_free(err_msg);
        return false;
    }

    return run_migrations();
}

// ── Schema migration with version tracking ───────────────

bool Database::run_migrations()
{
    // Read current version
    int version = 0;
    {
        SqliteStmt stmt(db_, "SELECT version FROM schema_version LIMIT 1");
        if (stmt && stmt.step() == SQLITE_ROW) {
            version = static_cast<int>(stmt.column_int64(0));
        }
    }

    if (version >= SchemaVersion::current) return true;

    spdlog::info("Database migration: v{} -> v{}", version, SchemaVersion::current);

    sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);

    // v0 -> v1: Add message file/image support
    if (version < 1) {
        const char* m[] = {
            "ALTER TABLE messages ADD COLUMN file_url TEXT DEFAULT ''",
            "ALTER TABLE messages ADD COLUMN file_name TEXT DEFAULT ''",
            "ALTER TABLE messages ADD COLUMN file_size INTEGER DEFAULT 0",
            "ALTER TABLE messages ADD COLUMN is_recalled INTEGER DEFAULT 0",
            "ALTER TABLE messages ADD COLUMN recalled_at INTEGER DEFAULT 0",
            "ALTER TABLE messages ADD COLUMN read_by TEXT DEFAULT '[]'",
            "ALTER TABLE users ADD COLUMN avatar_url TEXT DEFAULT ''",
        };
        for (auto* s : m) sqlite3_exec(db_, s, nullptr, nullptr, nullptr);
    }

    // v1 -> v2: Group persistence tables
    if (version < 2) {
        const char* m[] = {
            "CREATE TABLE IF NOT EXISTS groups_data (group_id TEXT PRIMARY KEY, group_name TEXT NOT NULL, owner_id TEXT NOT NULL, created_at INTEGER NOT NULL)",
            "CREATE TABLE IF NOT EXISTS group_members (group_id TEXT NOT NULL, user_id TEXT NOT NULL, role TEXT DEFAULT 'member', PRIMARY KEY (group_id, user_id))",
        };
        for (auto* s : m) sqlite3_exec(db_, s, nullptr, nullptr, nullptr);
    }

    // v4 -> v5: Session tokens (cookie-like fast re-auth)
    if (version < 5) {
        const char* m[] = {
            "CREATE TABLE IF NOT EXISTS session_tokens (token TEXT PRIMARY KEY, user_id TEXT NOT NULL, created_at INTEGER NOT NULL, expires_at INTEGER NOT NULL)",
            "CREATE INDEX IF NOT EXISTS idx_session_tokens_user ON session_tokens(user_id)",
        };
        for (auto* s : m) sqlite3_exec(db_, s, nullptr, nullptr, nullptr);
    }

    // v5 -> v6: Device management + read receipts
    if (version < 6) {
        const char* m[] = {
            "ALTER TABLE session_tokens ADD COLUMN device_id TEXT DEFAULT ''",
            "ALTER TABLE session_tokens ADD COLUMN device_name TEXT DEFAULT ''",
            "CREATE TABLE IF NOT EXISTS message_reads ("
                "message_id INTEGER NOT NULL, user_id TEXT NOT NULL, read_at INTEGER NOT NULL, "
                "PRIMARY KEY (message_id, user_id))",
            "CREATE INDEX IF NOT EXISTS idx_message_reads_msg ON message_reads(message_id)",
        };
        for (auto* s : m) sqlite3_exec(db_, s, nullptr, nullptr, nullptr);
    }

    // v6 -> v7: FTS5 full-text search + scheduled messages
    if (version < 7) {
        const char* m[] = {
            "CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5("
                "text, sender_nickname, content='messages', content_rowid='id', tokenize='unicode61')",
            "CREATE TRIGGER IF NOT EXISTS messages_fts_ai AFTER INSERT ON messages BEGIN "
                "INSERT INTO messages_fts(rowid, text, sender_nickname) VALUES (new.id, new.text, new.sender_nickname); END",
            "CREATE TRIGGER IF NOT EXISTS messages_fts_ad AFTER DELETE ON messages BEGIN "
                "INSERT INTO messages_fts(messages_fts, rowid, text, sender_nickname) VALUES('delete', old.id, old.text, old.sender_nickname); END",
            "CREATE TRIGGER IF NOT EXISTS messages_fts_au AFTER UPDATE ON messages BEGIN "
                "INSERT INTO messages_fts(messages_fts, rowid, text, sender_nickname) VALUES('delete', old.id, old.text, old.sender_nickname); "
                "INSERT INTO messages_fts(rowid, text, sender_nickname) VALUES (new.id, new.text, new.sender_nickname); END",
            "CREATE TABLE IF NOT EXISTS scheduled_messages ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT, sender_id TEXT NOT NULL, chat_type TEXT NOT NULL, "
                "chat_id TEXT NOT NULL, text TEXT NOT NULL, send_at INTEGER NOT NULL, "
                "created_at INTEGER NOT NULL, sent INTEGER DEFAULT 0)",
            "CREATE INDEX IF NOT EXISTS idx_scheduled_send ON scheduled_messages(send_at, sent)",
        };
        for (auto* s : m) sqlite3_exec(db_, s, nullptr, nullptr, nullptr);
    }
    if (version < 4) {
        const char* m[] = {
            "CREATE TABLE IF NOT EXISTS chat_read_cursors (chat_id TEXT NOT NULL, user_id TEXT NOT NULL, last_read_message_id INTEGER NOT NULL, read_at INTEGER NOT NULL, PRIMARY KEY (chat_id, user_id))",
            "CREATE INDEX IF NOT EXISTS idx_chat_read_cursors_chat ON chat_read_cursors(chat_id)",
        };
        for (auto* s : m) sqlite3_exec(db_, s, nullptr, nullptr, nullptr);
    }

    // Update version
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "UPDATE schema_version SET version = %d", SchemaVersion::current);
        sqlite3_exec(db_, buf, nullptr, nullptr, nullptr);
    }

    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    return true;
}

// --- Utilities ---

std::int64_t Database::current_timestamp() noexcept
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string Database::hash_password(const std::string& password)
{
    char hash[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(hash, password.c_str(), password.length(),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        sodium_memzero(hash, sizeof(hash));
        throw std::runtime_error("Failed to hash password");
    }
    std::string result(hash);
    sodium_memzero(hash, sizeof(hash));
    return result;
}

bool Database::verify_password(const std::string& password, const std::string& hash) noexcept
{
    return crypto_pwhash_str_verify(hash.c_str(), password.c_str(), password.length()) == 0;
}

// --- User operations ---

std::optional<UserRecord> Database::fetch_user_by_column(const char* column, const std::string& value)
{
    auto sql = std::string("SELECT user_id, username, password_hash, nickname, avatar_url, created_at FROM users WHERE ") + column + " = ?";
    SqliteStmt stmt(db_, sql.c_str(), true);
    if (!stmt) return std::nullopt;

    stmt.bind_text(1, value);

    if (stmt.step() == SQLITE_ROW) {
        UserRecord user;
        user.user_id       = stmt.column_text(0);
        user.username       = stmt.column_text(1);
        user.password_hash  = stmt.column_text(2);
        user.nickname       = stmt.column_text(3);
        user.avatar_url     = stmt.column_text(4);
        user.created_at     = stmt.column_int64(5);
        return user;
    }
    return std::nullopt;
}

bool Database::create_user(const std::string& username, const std::string& password, const std::string& nickname)
{
    std::lock_guard lock(db_mutex_);

    SqliteStmt stmt(db_, "INSERT INTO users (user_id, username, password_hash, nickname, created_at) VALUES (?, ?, ?, ?, ?)", true);
    if (!stmt) return false;

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    char uid_buf[16];
    std::snprintf(uid_buf, sizeof(uid_buf), "u_%08llx", static_cast<unsigned long long>(now % 0xFFFFFFFF));

    auto pw_hash = hash_password(password);

    stmt.bind_text(1, uid_buf);
    stmt.bind_text(2, username);
    stmt.bind_text(3, pw_hash);
    stmt.bind_text(4, nickname);
    stmt.bind_int64(5, current_timestamp());

    if (stmt.step() != SQLITE_DONE) {
        spdlog::error("Failed to create user: {}", sqlite3_errmsg(db_));
        return false;
    }

    UserRecord user{uid_buf, username, pw_hash, nickname, "", current_timestamp()};
    user_cache_.put("username:" + username, user);
    user_cache_.put("id:" + user.user_id, std::move(user));
    return true;
}

std::optional<UserRecord> Database::authenticate_user(const std::string& username, const std::string& password)
{
    if (auto cached = user_cache_.get("username:" + username)) {
        if (verify_password(password, cached->password_hash)) return cached;
    }

    std::lock_guard lock(db_mutex_);
    auto result = fetch_user_by_column("username", username);
    if (result && verify_password(password, result->password_hash)) {
        user_cache_.put("username:" + username, *result);
        user_cache_.put("id:" + result->user_id, *result);
        return result;
    }
    return std::nullopt;
}

std::optional<UserRecord> Database::find_user_by_id(std::string_view user_id)
{
    std::string uid(user_id);
    if (auto cached = user_cache_.get("id:" + uid)) return cached;

    std::lock_guard lock(db_mutex_);
    auto result = fetch_user_by_column("user_id", uid);
    if (result) {
        user_cache_.put("id:" + uid, *result);
        user_cache_.put("username:" + result->username, *result);
    }
    return result;
}

std::optional<UserRecord> Database::find_user_by_username(std::string_view username)
{
    std::string uname(username);
    if (auto cached = user_cache_.get("username:" + uname)) return cached;

    std::lock_guard lock(db_mutex_);
    auto result = fetch_user_by_column("username", uname);
    if (result) {
        user_cache_.put("username:" + uname, *result);
        user_cache_.put("id:" + result->user_id, *result);
    }
    return result;
}

bool Database::update_user_nickname(std::string_view user_id, std::string_view nickname)
{
    std::lock_guard lock(db_mutex_);

    SqliteStmt stmt(db_, "UPDATE users SET nickname = ? WHERE user_id = ?", true);
    if (!stmt) return false;

    std::string nick(nickname);
    std::string uid(user_id);
    stmt.bind_text(1, nick);
    stmt.bind_text(2, uid);

    if (stmt.step() == SQLITE_DONE) {
        user_cache_.remove("id:" + uid);
        return true;
    }
    return false;
}

bool Database::update_user_avatar(std::string_view user_id, std::string_view avatar_url)
{
    std::lock_guard lock(db_mutex_);

    SqliteStmt stmt(db_, "UPDATE users SET avatar_url = ? WHERE user_id = ?", true);
    if (!stmt) return false;

    std::string url(avatar_url);
    std::string uid(user_id);
    stmt.bind_text(1, url);
    stmt.bind_text(2, uid);

    if (stmt.step() == SQLITE_DONE) {
        user_cache_.remove("id:" + uid);
        return true;
    }
    return false;
}

// --- Message operations ---

bool Database::save_message(std::string_view sender_id, std::string_view sender_nickname,
                            std::string_view receiver_id, std::string_view text,
                            std::string_view message_type,
                            std::string_view file_url, std::string_view file_name,
                            std::int64_t file_size)
{
    int rc;
    std::string sid(sender_id), snick(sender_nickname), rid(receiver_id);
    std::string txt(text), mtype(message_type), furl(file_url), fname(file_name);
    {
        std::lock_guard lock(db_mutex_);

        SqliteStmt stmt(db_, "INSERT INTO messages (sender_id, sender_nickname, receiver_id, text, message_type, file_url, file_name, file_size, timestamp) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", true);
        if (!stmt) return false;

        stmt.bind_text(1, sid);
        stmt.bind_text(2, snick);
        stmt.bind_text(3, rid);
        stmt.bind_text(4, txt);
        stmt.bind_text(5, mtype);
        stmt.bind_text(6, furl);
        stmt.bind_text(7, fname);
        stmt.bind_int64(8, file_size);
        stmt.bind_int64(9, current_timestamp());

        rc = stmt.step();
    }

    if (rc == SQLITE_DONE) {
        message_cache_.remove("recent");
        if (!rid.empty()) {
            message_cache_.remove("private:" + std::min(sid, rid) + ":" + std::max(sid, rid));
            message_cache_.remove("group:" + rid);
        }
        return true;
    }
    return false;
}

MessageRecord Database::read_message_row(const SqliteStmt& stmt) const noexcept
{
    MessageRecord msg;
    msg.id              = stmt.column_int64(0);
    msg.sender_id       = stmt.column_text(1);
    msg.sender_nickname = stmt.column_text(2);
    msg.receiver_id     = stmt.column_text(3);
    msg.text            = stmt.column_text(4);
    msg.message_type    = stmt.column_text(5);
    msg.file_url        = stmt.column_text(6);
    msg.file_name       = stmt.column_text(7);
    msg.file_size       = stmt.column_int64(8);
    msg.is_recalled     = stmt.column_int64(9) != 0;
    msg.recalled_at     = stmt.column_int64(10);
    msg.read_by         = stmt.column_text(11);
    msg.timestamp       = stmt.column_int64(12);
    return msg;
}

std::vector<MessageRecord> Database::get_recent_messages(int limit)
{
    if (auto cached = message_cache_.get("recent")) {
        if (cached->size() <= static_cast<std::size_t>(limit)) return *cached;
    }

    std::lock_guard lock(db_mutex_);

    SqliteStmt stmt(db_,
        "SELECT id, sender_id, sender_nickname, receiver_id, text, message_type, "
        "file_url, file_name, file_size, is_recalled, recalled_at, read_by, timestamp "
        "FROM messages WHERE receiver_id = '' ORDER BY timestamp DESC LIMIT ?", true);
    if (!stmt) return {};

    stmt.bind_int(1, limit);

    std::vector<MessageRecord> messages;
    messages.reserve(limit);
    while (stmt.step() == SQLITE_ROW) {
        messages.push_back(read_message_row(stmt));
    }
    std::reverse(messages.begin(), messages.end());

    message_cache_.put("recent", messages);
    return messages;
}

std::vector<MessageRecord> Database::get_private_messages(std::string_view user1_id, std::string_view user2_id, int limit)
{
    std::string u1(user1_id), u2(user2_id);
    auto cache_key = "private:" + std::min(u1, u2) + ":" + std::max(u1, u2);
    if (auto cached = message_cache_.get(cache_key)) {
        if (cached->size() <= static_cast<std::size_t>(limit)) return *cached;
    }

    std::lock_guard lock(db_mutex_);

    SqliteStmt stmt(db_,
        "SELECT id, sender_id, sender_nickname, receiver_id, text, message_type, "
        "file_url, file_name, file_size, is_recalled, recalled_at, read_by, timestamp "
        "FROM messages WHERE (sender_id = ? AND receiver_id = ?) OR (sender_id = ? AND receiver_id = ?) "
        "ORDER BY timestamp DESC LIMIT ?", true);
    if (!stmt) return {};

    stmt.bind_text(1, u1);
    stmt.bind_text(2, u2);
    stmt.bind_text(3, u2);
    stmt.bind_text(4, u1);
    stmt.bind_int(5, limit);

    std::vector<MessageRecord> messages;
    messages.reserve(limit);
    while (stmt.step() == SQLITE_ROW) {
        messages.push_back(read_message_row(stmt));
    }
    std::reverse(messages.begin(), messages.end());

    message_cache_.put(cache_key, messages);
    return messages;
}

std::vector<MessageRecord> Database::get_group_messages(std::string_view group_id, int limit)
{
    std::string gid(group_id);
    auto cache_key = "group:" + gid;
    if (auto cached = message_cache_.get(cache_key)) {
        if (cached->size() <= static_cast<std::size_t>(limit)) return *cached;
    }

    std::lock_guard lock(db_mutex_);

    SqliteStmt stmt(db_,
        "SELECT id, sender_id, sender_nickname, receiver_id, text, message_type, "
        "file_url, file_name, file_size, is_recalled, recalled_at, read_by, timestamp "
        "FROM messages WHERE receiver_id = ? AND message_type = 'group' "
        "ORDER BY timestamp DESC LIMIT ?", true);
    if (!stmt) return {};

    stmt.bind_text(1, gid);
    stmt.bind_int(2, limit);

    std::vector<MessageRecord> messages;
    messages.reserve(limit);
    while (stmt.step() == SQLITE_ROW) {
        messages.push_back(read_message_row(stmt));
    }
    std::reverse(messages.begin(), messages.end());

    message_cache_.put(cache_key, messages);
    return messages;
}

bool Database::recall_message(std::int64_t message_id, std::string_view user_id)
{
    std::lock_guard lock(db_mutex_);

    SqliteStmt stmt(db_, "UPDATE messages SET is_recalled = 1, recalled_at = ? WHERE id = ? AND sender_id = ?", true);
    if (!stmt) return false;

    std::string uid(user_id);
    stmt.bind_int64(1, current_timestamp());
    stmt.bind_int64(2, message_id);
    stmt.bind_text(3, uid);

    if (stmt.step() == SQLITE_DONE) {
        message_cache_.remove("recent");
        return true;
    }
    return false;
}

// --- Edit & Delete ---

bool Database::edit_message(std::int64_t message_id, std::string_view user_id, std::string_view new_text)
{
    std::lock_guard lock(db_mutex_);

    SqliteStmt stmt(db_, "UPDATE messages SET text = ? WHERE id = ? AND sender_id = ? AND is_recalled = 0", true);
    if (!stmt) return false;

    std::string uid(user_id), txt(new_text);
    stmt.bind_text(1, txt);
    stmt.bind_int64(2, message_id);
    stmt.bind_text(3, uid);

    if (stmt.step() == SQLITE_DONE) {
        message_cache_.remove("recent");
        return true;
    }
    return false;
}

bool Database::delete_message(std::int64_t message_id, std::string_view user_id)
{
    std::lock_guard lock(db_mutex_);

    SqliteStmt stmt(db_, "DELETE FROM messages WHERE id = ? AND sender_id = ?", true);
    if (!stmt) return false;

    std::string uid(user_id);
    stmt.bind_int64(1, message_id);
    stmt.bind_text(2, uid);

    if (stmt.step() == SQLITE_DONE) {
        message_cache_.remove("recent");
        return true;
    }
    return false;
}

// --- Pagination ---

std::vector<MessageRecord> Database::get_messages_before(std::int64_t before_ts, int limit)
{
    std::lock_guard lock(db_mutex_);

    SqliteStmt stmt(db_,
        "SELECT id, sender_id, sender_nickname, receiver_id, text, message_type, "
        "file_url, file_name, file_size, is_recalled, recalled_at, read_by, timestamp "
        "FROM messages WHERE receiver_id = '' AND timestamp < ? ORDER BY timestamp DESC LIMIT ?", true);
    if (!stmt) return {};

    stmt.bind_int64(1, before_ts);
    stmt.bind_int(2, limit);

    std::vector<MessageRecord> messages;
    messages.reserve(limit);
    while (stmt.step() == SQLITE_ROW) {
        messages.push_back(read_message_row(stmt));
    }
    std::reverse(messages.begin(), messages.end());
    return messages;
}

// --- Pin messages ---

bool Database::pin_message(std::int64_t message_id, std::string_view chat_id, std::string_view user_id)
{
    std::lock_guard lock(db_mutex_);

    // Create table if not exists
    sqlite3_exec(db_,
        "CREATE TABLE IF NOT EXISTS pinned_messages ("
        "message_id INTEGER NOT NULL, chat_id TEXT NOT NULL, "
        "pinned_by TEXT NOT NULL, pinned_at INTEGER NOT NULL, "
        "PRIMARY KEY (message_id, chat_id))",
        nullptr, nullptr, nullptr);

    SqliteStmt stmt(db_,
        "INSERT OR REPLACE INTO pinned_messages (message_id, chat_id, pinned_by, pinned_at) "
        "VALUES (?, ?, ?, ?)", true);
    if (!stmt) return false;

    std::string cid(chat_id), uid(user_id);
    stmt.bind_int64(1, message_id);
    stmt.bind_text(2, cid);
    stmt.bind_text(3, uid);
    stmt.bind_int64(4, current_timestamp());

    return stmt.step() == SQLITE_DONE;
}

bool Database::unpin_message(std::int64_t message_id, std::string_view chat_id)
{
    std::lock_guard lock(db_mutex_);

    SqliteStmt stmt(db_, "DELETE FROM pinned_messages WHERE message_id = ? AND chat_id = ?", true);
    if (!stmt) return false;

    std::string cid(chat_id);
    stmt.bind_int64(1, message_id);
    stmt.bind_text(2, cid);

    return stmt.step() == SQLITE_DONE;
}

bool Database::unpin_chat_messages(std::string_view chat_id)
{
    std::lock_guard lock(db_mutex_);

    SqliteStmt stmt(db_, "DELETE FROM pinned_messages WHERE chat_id = ?", true);
    if (!stmt) return false;

    std::string cid(chat_id);
    stmt.bind_text(1, cid);

    return stmt.step() == SQLITE_DONE;
}

std::vector<Database::PinnedRecord> Database::get_pinned_messages(std::string_view chat_id)
{
    std::lock_guard lock(db_mutex_);

    // Ensure table exists
    sqlite3_exec(db_,
        "CREATE TABLE IF NOT EXISTS pinned_messages ("
        "message_id INTEGER NOT NULL, chat_id TEXT NOT NULL, "
        "pinned_by TEXT NOT NULL, pinned_at INTEGER NOT NULL, "
        "PRIMARY KEY (message_id, chat_id))",
        nullptr, nullptr, nullptr);

    SqliteStmt stmt(db_,
        "SELECT message_id, chat_id, pinned_by, pinned_at "
        "FROM pinned_messages WHERE chat_id = ? ORDER BY pinned_at DESC", true);
    if (!stmt) return {};

    std::string cid(chat_id);
    stmt.bind_text(1, cid);

    std::vector<PinnedRecord> results;
    results.reserve(16); // Reasonable default for pinned messages
    while (stmt.step() == SQLITE_ROW) {
        results.emplace_back(PinnedRecord{
            .message_id = stmt.column_int64(0),
            .pinned_at  = stmt.column_int64(3),
            .chat_id    = stmt.column_text(1),
            .pinned_by  = stmt.column_text(2),
        });
    }
    return results;
}

// --- Saved messages ---

bool Database::save_message_to_collection(std::int64_t message_id, std::string_view chat_id, std::string_view user_id)
{
    std::lock_guard lock(db_mutex_);

    // Create table if not exists
    sqlite3_exec(db_,
        "CREATE TABLE IF NOT EXISTS saved_messages ("
        "message_id INTEGER NOT NULL, chat_id TEXT NOT NULL, "
        "user_id TEXT NOT NULL, saved_at INTEGER NOT NULL, "
        "PRIMARY KEY (message_id, user_id))",
        nullptr, nullptr, nullptr);

    SqliteStmt stmt(db_,
        "INSERT OR REPLACE INTO saved_messages (message_id, chat_id, user_id, saved_at) "
        "VALUES (?, ?, ?, ?)", true);
    if (!stmt) return false;

    std::string cid(chat_id), uid(user_id);
    stmt.bind_int64(1, message_id);
    stmt.bind_text(2, cid);
    stmt.bind_text(3, uid);
    stmt.bind_int64(4, current_timestamp());

    return stmt.step() == SQLITE_DONE;
}

bool Database::unsave_message_from_collection(std::int64_t message_id, std::string_view user_id)
{
    std::lock_guard lock(db_mutex_);

    SqliteStmt stmt(db_, "DELETE FROM saved_messages WHERE message_id = ? AND user_id = ?", true);
    if (!stmt) return false;

    std::string uid(user_id);
    stmt.bind_int64(1, message_id);
    stmt.bind_text(2, uid);

    return stmt.step() == SQLITE_DONE;
}

std::vector<Database::SavedRecord> Database::get_saved_messages(std::string_view user_id)
{
    std::lock_guard lock(db_mutex_);

    // Ensure table exists
    sqlite3_exec(db_,
        "CREATE TABLE IF NOT EXISTS saved_messages ("
        "message_id INTEGER NOT NULL, chat_id TEXT NOT NULL, "
        "user_id TEXT NOT NULL, saved_at INTEGER NOT NULL, "
        "PRIMARY KEY (message_id, user_id))",
        nullptr, nullptr, nullptr);

    SqliteStmt stmt(db_,
        "SELECT message_id, chat_id, user_id, saved_at "
        "FROM saved_messages WHERE user_id = ? ORDER BY saved_at DESC", true);
    if (!stmt) return {};

    std::string uid(user_id);
    stmt.bind_text(1, uid);

    std::vector<SavedRecord> results;
    results.reserve(32); // Reasonable default for saved messages
    while (stmt.step() == SQLITE_ROW) {
        results.emplace_back(SavedRecord{
            .message_id = stmt.column_int64(0),
            .saved_at   = stmt.column_int64(3),
            .chat_id    = stmt.column_text(1),
            .user_id    = stmt.column_text(2),
        });
    }
    return results;
}

// ── Direct message lookup ─────────────────────────────────

std::optional<MessageRecord> Database::find_message_by_id(std::int64_t message_id)
{
    std::lock_guard lock(db_mutex_);

    SqliteStmt stmt(db_, "SELECT * FROM messages WHERE id = ?", true);
    if (!stmt) return std::nullopt;

    stmt.bind_int64(1, message_id);
    if (stmt.step() == SQLITE_ROW) {
        return read_message_row(stmt);
    }
    return std::nullopt;
}

// ── Group persistence ─────────────────────────────────────

bool Database::create_group(std::string_view group_id, std::string_view group_name, std::string_view owner_id)
{
    std::lock_guard lock(db_mutex_);
    SqliteStmt stmt(db_,
        "INSERT INTO groups_data (group_id, group_name, owner_id, created_at) VALUES (?,?,?,?)", true);
    if (!stmt) return false;

    std::string gid(group_id), gname(group_name), oid(owner_id);
    stmt.bind_text(1, gid);
    stmt.bind_text(2, gname);
    stmt.bind_text(3, oid);
    stmt.bind_int64(4, current_timestamp());
    return stmt.step() == SQLITE_DONE;
}


bool Database::add_group_member(std::string_view group_id, std::string_view user_id)
{
    std::lock_guard lock(db_mutex_);
    SqliteStmt stmt(db_,
        "INSERT OR IGNORE INTO group_members (group_id, user_id, role) VALUES (?,?,'member')", true);
    if (!stmt) return false;
    std::string gid(group_id), uid(user_id);
    stmt.bind_text(1, gid);
    stmt.bind_text(2, uid);
    return stmt.step() == SQLITE_DONE;
}

bool Database::remove_group_member(std::string_view group_id, std::string_view user_id)
{
    std::lock_guard lock(db_mutex_);
    SqliteStmt stmt(db_, "DELETE FROM group_members WHERE group_id = ? AND user_id = ?", true);
    if (!stmt) return false;
    std::string gid(group_id), uid(user_id);
    stmt.bind_text(1, gid);
    stmt.bind_text(2, uid);
    return stmt.step() == SQLITE_DONE;
}

// ── Restored implementations ──────────────────────────────

bool Database::set_session_token(std::string_view user_id, std::string_view token,
                                 std::string_view device_id, std::string_view device_name) {
    std::lock_guard lock(db_mutex_);
    SqliteStmt stmt(db_, "INSERT OR REPLACE INTO session_tokens "
                    "(token, user_id, created_at, expires_at, device_id, device_name) VALUES (?,?,?,?,?,?)", true);
    if (!stmt) return false;
    std::string t(token), uid(user_id), did(device_id), dn(device_name);
    auto now = current_timestamp();
    stmt.bind_text(1, t); stmt.bind_text(2, uid);
    stmt.bind_int64(3, now); stmt.bind_int64(4, now + 86400 * 30);
    stmt.bind_text(5, did); stmt.bind_text(6, dn);
    return stmt.step() == SQLITE_DONE;
}

std::optional<Database::SessionInfo> Database::find_session_by_token(std::string_view token) {
    std::lock_guard lock(db_mutex_);
    SqliteStmt stmt(db_, "SELECT user_id, device_id, device_name, expires_at FROM session_tokens "
                    "WHERE token = ? AND expires_at > ?", true);
    if (!stmt) return std::nullopt;
    std::string t(token);
    stmt.bind_text(1, t);
    stmt.bind_int64(2, current_timestamp());
    if (stmt.step() != SQLITE_ROW) return std::nullopt;
    SessionInfo info;
    info.user_id = stmt.column_text(0);
    info.device_id = stmt.column_text(1);
    info.device_name = stmt.column_text(2);
    info.expires_at = stmt.column_int64(3);
    return info;
}

bool Database::renew_session_token(std::string_view token) {
    std::lock_guard lock(db_mutex_);
    SqliteStmt stmt(db_, "UPDATE session_tokens SET expires_at = ? WHERE token = ?", true);
    if (!stmt) return false;
    std::string t(token);
    stmt.bind_int64(1, current_timestamp() + 86400 * 30);
    stmt.bind_text(2, t);
    return stmt.step() == SQLITE_DONE;
}

std::vector<Database::SessionInfo> Database::get_user_sessions(std::string_view user_id) {
    std::lock_guard lock(db_mutex_);
    SqliteStmt stmt(db_, "SELECT token, device_id, device_name, created_at, expires_at FROM session_tokens "
                    "WHERE user_id = ? AND expires_at > ? ORDER BY created_at DESC", true);
    if (!stmt) return {};
    std::string uid(user_id);
    stmt.bind_text(1, uid);
    stmt.bind_int64(2, current_timestamp());
    std::vector<SessionInfo> sessions;
    while (stmt.step() == SQLITE_ROW) {
        SessionInfo info;
        info.token = stmt.column_text(0);
        info.device_id = stmt.column_text(1);
        info.device_name = stmt.column_text(2);
        info.created_at = stmt.column_int64(3);
        info.expires_at = stmt.column_int64(4);
        sessions.push_back(std::move(info));
    }
    return sessions;
}

bool Database::remove_device_session(std::string_view token) {
    std::lock_guard lock(db_mutex_);
    SqliteStmt stmt(db_, "DELETE FROM session_tokens WHERE token = ?", true);
    if (!stmt) return false;
    std::string t(token);
    stmt.bind_text(1, t);
    return stmt.step() == SQLITE_DONE;
}

// ── Read receipt tracking ────────────────────────────────

bool Database::record_message_read(std::int64_t message_id, std::string_view user_id) {
    std::lock_guard lock(db_mutex_);
    SqliteStmt stmt(db_, "INSERT OR IGNORE INTO message_reads (message_id, user_id, read_at) VALUES (?,?,?)", true);
    if (!stmt) return false;
    std::string uid(user_id);
    stmt.bind_int64(1, message_id);
    stmt.bind_text(2, uid);
    stmt.bind_int64(3, current_timestamp());
    return stmt.step() == SQLITE_DONE;
}

std::vector<std::string> Database::get_message_readers(std::int64_t message_id) {
    std::lock_guard lock(db_mutex_);
    SqliteStmt stmt(db_, "SELECT user_id FROM message_reads WHERE message_id = ?", true);
    if (!stmt) return {};
    stmt.bind_int64(1, message_id);
    std::vector<std::string> readers;
    while (stmt.step() == SQLITE_ROW) {
        readers.emplace_back(stmt.column_text(0));
    }
    return readers;
}

// ── FTS5 Full-text Search (v2.3) ──────────────────────────

std::vector<Database::SearchResult> Database::search_messages(std::string_view query, int limit, int offset) {
    std::lock_guard lock(db_mutex_);
    // Simple query: FTS5 MATCH with rank sorting
    SqliteStmt stmt(db_,
        "SELECT m.id, m.sender_id, m.sender_nickname, m.text, m.receiver_id, "
        "m.message_type, m.timestamp, rank FROM messages_fts f "
        "JOIN messages m ON f.rowid = m.id "
        "WHERE messages_fts MATCH ? ORDER BY rank LIMIT ? OFFSET ?", true);
    if (!stmt) return {};

    std::string q(query);
    stmt.bind_text(1, q);
    stmt.bind_int(2, limit);
    stmt.bind_int(3, offset);

    std::vector<SearchResult> results;
    while (stmt.step() == SQLITE_ROW) {
        SearchResult r;
        r.message_id = stmt.column_int64(0);
        r.sender_id = stmt.column_text(1);
        r.sender_nickname = stmt.column_text(2);
        r.text = stmt.column_text(3);
        r.receiver_id = stmt.column_text(4);
        r.message_type = stmt.column_text(5);
        r.timestamp = stmt.column_int64(6);
        r.rank = static_cast<double>(stmt.column_int64(7));
        results.push_back(std::move(r));
    }
    return results;
}

// ── Scheduled Messages (v2.3) ─────────────────────────────

std::int64_t Database::schedule_message(std::string_view sender_id, std::string_view chat_type,
                                         std::string_view chat_id, std::string_view text, std::int64_t send_at) {
    std::lock_guard lock(db_mutex_);
    SqliteStmt stmt(db_,
        "INSERT INTO scheduled_messages (sender_id, chat_type, chat_id, text, send_at, created_at) "
        "VALUES (?,?,?,?,?,?)", true);
    if (!stmt) return -1;
    std::string sid(sender_id), ct(chat_type), cid(chat_id), t(text);
    stmt.bind_text(1, sid); stmt.bind_text(2, ct); stmt.bind_text(3, cid);
    stmt.bind_text(4, t); stmt.bind_int64(5, send_at);
    stmt.bind_int64(6, current_timestamp());
    if (stmt.step() != SQLITE_DONE) return -1;
    return sqlite3_last_insert_rowid(db_);
}

std::vector<MessageRecord> Database::get_due_scheduled_messages(std::int64_t before_ts) {
    std::lock_guard lock(db_mutex_);
    SqliteStmt stmt(db_,
        "SELECT id, sender_id, chat_type, chat_id, text FROM scheduled_messages "
        "WHERE send_at <= ? AND sent = 0 ORDER BY send_at ASC LIMIT 100", true);
    if (!stmt) return {};
    stmt.bind_int64(1, before_ts);
    std::vector<MessageRecord> records;
    while (stmt.step() == SQLITE_ROW) {
        MessageRecord r;
        r.id = stmt.column_int64(0);
        r.sender_id = stmt.column_text(1);
        r.text = stmt.column_text(4);
        r.timestamp = before_ts;
        records.push_back(std::move(r));
    }
    return records;
}

bool Database::mark_scheduled_sent(std::int64_t schedule_id) {
    std::lock_guard lock(db_mutex_);
    SqliteStmt stmt(db_, "UPDATE scheduled_messages SET sent = 1 WHERE id = ?", true);
    if (!stmt) return false;
    stmt.bind_int64(1, schedule_id);
    return stmt.step() == SQLITE_DONE;
}

bool Database::clear_session_token(std::string_view user_id) {
    std::lock_guard lock(db_mutex_);
    SqliteStmt stmt(db_, "DELETE FROM session_tokens WHERE user_id = ?", true);
    if (!stmt) return false;
    std::string uid(user_id);
    stmt.bind_text(1, uid);
    return stmt.step() == SQLITE_DONE;
}

bool Database::mark_chat_read(std::string_view chat_id, std::string_view user_id, std::int64_t last_read_id) {
    std::lock_guard lock(db_mutex_);
    SqliteStmt stmt(db_, "INSERT OR REPLACE INTO chat_read_cursors (chat_id, user_id, last_read_message_id, read_at) VALUES (?,?,?,?)", true);
    if (!stmt) return false;
    std::string cid(chat_id), uid(user_id);
    stmt.bind_text(1, cid); stmt.bind_text(2, uid);
    stmt.bind_int64(3, last_read_id); stmt.bind_int64(4, current_timestamp());
    return stmt.step() == SQLITE_DONE;
}

std::int64_t Database::get_read_cursor(std::string_view chat_id, std::string_view user_id) {
    std::lock_guard lock(db_mutex_);
    SqliteStmt stmt(db_, "SELECT last_read_message_id FROM chat_read_cursors WHERE chat_id = ? AND user_id = ?", true);
    if (!stmt) return 0;
    std::string cid(chat_id), uid(user_id);
    stmt.bind_text(1, cid); stmt.bind_text(2, uid);
    if (stmt.step() != SQLITE_ROW) return 0;
    return stmt.column_int64(0);
}

int Database::get_unread_count(std::string_view chat_id, std::string_view user_id, std::int64_t since) {
    std::lock_guard lock(db_mutex_);
    SqliteStmt stmt(db_, "SELECT COUNT(*) FROM messages WHERE receiver_id = ? AND id > ?", true);
    if (!stmt) return 0;
    std::string cid(chat_id);
    stmt.bind_text(1, cid); stmt.bind_int64(2, since);
    if (stmt.step() != SQLITE_ROW) return 0;
    return static_cast<int>(stmt.column_int64(0));
}

bool Database::save_messages_batch(const std::vector<MessageRecord>& msgs) {
    std::lock_guard lock(db_mutex_);
    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    SqliteStmt stmt(db_,
        "INSERT INTO messages (timestamp, sender_id, sender_nickname, receiver_id, text, message_type, file_url, file_name, file_size) VALUES (?,?,?,?,?,?,?,?,?)", false);
    if (!stmt) { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); return false; }
    for (auto& m : msgs) {
        stmt.bind_int64(1, m.timestamp); stmt.bind_text(2, m.sender_id);
        stmt.bind_text(3, m.sender_nickname); stmt.bind_text(4, m.receiver_id);
        stmt.bind_text(5, m.text); stmt.bind_text(6, m.message_type);
        stmt.bind_text(7, m.file_url); stmt.bind_text(8, m.file_name);
        stmt.bind_int64(9, m.file_size);
        if (stmt.step() != SQLITE_DONE) { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); return false; }
        sqlite3_reset(stmt.get());
    }
    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    return true;
}

} // namespace imchat
