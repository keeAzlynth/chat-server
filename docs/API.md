# IM Chat 后端 WebSocket API 文档

> 版本：v2.1  
> 更新日期：2026-06-28  
> 技术栈：C++20 / Boost.Beast / SQLite / FNV-1a哈希分发

所有通信通过 WebSocket 连接完成。客户端发送 JSON 消息，服务端返回 JSON 消息。

**通用格式：**

```json
// 请求（客户端 → 服务端）
{ "type": "<消息类型>", ... }

// 响应（服务端 → 客户端）
{ "type": "<响应类型>", ... }
```

---

## 目录

- [一、认证（Auth）](#一认证auth)
- [二、聊天室（Room）](#二聊天室room)
- [三、消息管理（Message Management）](#三消息管理message-management)
- [四、置顶与收藏（Pin & Save）](#四置顶与收藏pin--save)
- [五、群组（Group）](#五群组group)
- [六、查询（Query）](#六查询query)
- [七、错误码汇总](#七错误码汇总)
- [八、用户等级与权限](#八用户等级与权限)
- [九、典型交互流程](#九典型交互流程)
- [十、技术实现说明](#十技术实现说明)

---

## 一、认证（Auth）

### 1.1 注册 `register`

**请求：**
```json
{
  "type": "register",
  "username": "string",   // 用户名
  "password": "string",   // 密码
  "nickname": "string"    // 昵称
}
```

**成功响应：**
```json
{
  "type": "logged_in",
  "userId": "u_XXXXXXXX",
  "username": "string",
  "nickname": "string",
  "level": 1
}
```

**错误响应：**
| code | 含义 |
|------|------|
| `USERNAME_EXISTS` | 用户名已存在 |
| `INVALID_FORMAT` | 字段为空或格式错误 |

---

### 1.2 登录 `login`

**请求：**
```json
{
  "type": "login",
  "username": "string",
  "password": "string"
}
```

**成功响应：**
```json
{
  "type": "logged_in",
  "userId": "u_XXXXXXXX",
  "username": "string",
  "nickname": "string",
  "level": 1
}
```

**错误响应：**
| code | 含义 |
|------|------|
| `INVALID_CREDENTIALS` | 用户名或密码错误 |
| `ALREADY_LOGGED_IN` | 用户已在线 |
| `INVALID_FORMAT` | 字段为空 |

---

### 1.3 登出 `logout`

**请求：**
```json
{
  "type": "logout"
}
```

**响应：**
```json
{
  "type": "logged_out"
}
```

---

## 二、聊天室（Room）

### 2.1 加入聊天室 `join`

**请求：**
```json
{
  "type": "join",
  "nickname": "string"
}
```

**响应（发送给本人）：**
```json
{
  "type": "joined",
  "userId": "g_XXXXXXXX",
  "nickname": "string",
  "level": 0
}
```

**广播（发送给所有人，包括本人）：**
```json
{
  "type": "online_users",
  "users": [
    { "userId": "string", "nickname": "string", "level": 0 }
  ]
}
```

> 注：如果用户已登录（login 后），join 仅更新昵称，不会创建新的 guest 用户。

**错误响应：**
| code | 含义 |
|------|------|
| `NICKNAME_TAKEN` | 昵称已被占用 |
| `INVALID_FORMAT` | 昵称为空 |

---

### 2.2 发送消息 `send`

**请求：**
```json
{
  "type": "send",
  "text": "string"
}
```

**广播（发送给所有人）：**
```json
{
  "type": "message",
  "userId": "string",
  "nickname": "string",
  "text": "string",
  "ts": 1719000000,
  "level": 1
}
```

**错误响应：**
| code | 含义 |
|------|------|
| `NOT_JOINED` | 未加入聊天室 |
| `MESSAGE_TOO_LONG` | 消息超过长度限制 |
| `INVALID_FORMAT` | 消息为空 |

---

### 2.3 发送文件消息 `send_file`

**请求：**
```json
{
  "type": "send_file",
  "text": "string",         // 文件描述（可为空）
  "fileUrl": "string",      // 文件 URL
  "fileName": "string",     // 文件名
  "fileSize": 1024          // 文件大小（字节）
}
```

**广播：**
```json
{
  "type": "message",
  "userId": "string",
  "nickname": "string",
  "text": "string",
  "messageType": "file",
  "fileUrl": "string",
  "fileName": "string",
  "fileSize": 1024,
  "ts": 1719000000
}
```

---

### 2.4 发送私聊消息 `private_message`

**请求：**
```json
{
  "type": "private_message",
  "receiverId": "string",   // 目标用户 ID
  "text": "string"
}
```

**发送给发送者和接收者：**
```json
{
  "type": "private_message",
  "userId": "string",        // 发送者 ID
  "nickname": "string",      // 发送者昵称
  "receiverId": "string",    // 接收者 ID
  "text": "string",
  "ts": 1719000000,
  "level": 1
}
```

**错误响应：**
| code | 含义 |
|------|------|
| `NOT_JOINED` | 未加入聊天室 |
| `PERMISSION_DENIED` | 无发送私聊权限（Guest） |
| `USER_NOT_FOUND` | 目标用户不存在 |
| `MESSAGE_TOO_LONG` | 消息超过长度限制 |

---

### 2.5 正在输入 `typing`

**请求：**
```json
{
  "type": "typing"
}
```

**广播（发送给除自己外所有人）：**
```json
{
  "type": "typing",
  "userId": "string",
  "nickname": "string"
}
```

---

## 三、消息管理（Message Management）

### 3.1 撤回消息 `recall_message`

**请求：**
```json
{
  "type": "recall_message",
  "messageId": 12345
}
```

**广播：**
```json
{
  "type": "message_recalled",
  "messageId": 12345,
  "userId": "string"
}
```

**错误响应：**
| code | 含义 |
|------|------|
| `NOT_JOINED` | 未加入聊天室 |
| `PERMISSION_DENIED` | 非消息发送者，无权撤回 |

---

### 3.2 标记已读 `mark_read`

**请求：**
```json
{
  "type": "mark_read",
  "messageId": 12345
}
```

**响应（仅发送给本人）：**
```json
{
  "type": "message_read",
  "messageId": 12345,
  "userId": "string"
}
```

---

### 3.3 编辑消息 `edit_message`

**请求：**
```json
{
  "type": "edit_message",
  "messageId": 12345,
  "text": "string"           // 新消息内容
}
```

**广播：**
```json
{
  "type": "message_edited",
  "messageId": 12345,
  "userId": "string",
  "text": "string",
  "ts": 1719000000
}
```

**错误响应：**
| code | 含义 |
|------|------|
| `NOT_JOINED` | 未加入聊天室 |
| `PERMISSION_DENIED` | 非消息发送者，无权编辑 |
| `MESSAGE_TOO_LONG` | 消息超过长度限制 |

---

### 3.4 删除消息 `delete_message`

**请求：**
```json
{
  "type": "delete_message",
  "messageId": 12345
}
```

**广播：**
```json
{
  "type": "message_deleted",
  "messageId": 12345,
  "userId": "string"
}
```

**错误响应：**
| code | 含义 |
|------|------|
| `NOT_JOINED` | 未加入聊天室 |
| `PERMISSION_DENIED` | 非消息发送者，无权删除 |

---

## 四、置顶与收藏（Pin & Save）

### 4.1 置顶消息 `pin_message`

**请求：**
```json
{
  "type": "pin_message",
  "messageId": 12345,
  "chatId": "string"         // 聊天ID（public/private_XXX/group_XXX）
}
```

**广播：**
```json
{
  "type": "message_pinned",
  "messageId": 12345,
  "chatId": "string",
  "pinnedBy": "string",
  "ts": 1719000000
}
```

---

### 4.2 取消置顶 `unpin_message`

**请求：**
```json
{
  "type": "unpin_message",
  "messageId": 12345,
  "chatId": "string"
}
```

**广播：**
```json
{
  "type": "message_unpinned",
  "messageId": 12345,
  "chatId": "string"
}
```

---

### 4.3 获取置顶消息 `get_pinned_messages`

**请求：**
```json
{
  "type": "get_pinned_messages",
  "chatId": "string"
}
```

**响应（仅发送给本人）：**
```json
{
  "type": "pinned_messages",
  "chatId": "string",
  "messages": [
    {
      "userId": "string",
      "nickname": "string",
      "text": "string",
      "ts": 1719000000,
      "pinnedBy": "string",
      "pinnedAt": 1719000000
    }
  ]
}
```

---

### 4.4 收藏消息 `save_message`

**请求：**
```json
{
  "type": "save_message",
  "messageId": 12345,
  "chatId": "string"
}
```

**响应（仅发送给本人）：**
```json
{
  "type": "message_saved",
  "messageId": 12345,
  "chatId": "string"
}
```

---

### 4.5 取消收藏 `unsave_message`

**请求：**
```json
{
  "type": "unsave_message",
  "messageId": 12345
}
```

**响应（仅发送给本人）：**
```json
{
  "type": "message_unsaved",
  "messageId": 12345
}
```

---

### 4.6 获取收藏消息 `get_saved_messages`

**请求：**
```json
{
  "type": "get_saved_messages"
}
```

**响应（仅发送给本人）：**
```json
{
  "type": "saved_messages",
  "messages": [
    {
      "userId": "string",
      "nickname": "string",
      "text": "string",
      "ts": 1719000000,
      "savedAt": 1719000000,
      "chatId": "string"
    }
  ]
}
```

---

## 五、群组（Group）

### 5.1 创建群组 `create_group`

**请求：**
```json
{
  "type": "create_group",
  "name": "string"
}
```

**响应（仅发送给本人）：**
```json
{
  "type": "group_created",
  "groupId": "grp_XXXXXXXX",
  "name": "string",
  "ownerId": "string"
}
```

**错误响应：**
| code | 含义 |
|------|------|
| `NOT_JOINED` | 未加入聊天室 |
| `PERMISSION_DENIED` | 权限不足（Guest / Member 无创建权限） |
| `INVALID_FORMAT` | 群名为空 |

---

### 5.2 加入群组 `join_group`

**请求：**
```json
{
  "type": "join_group",
  "groupId": "grp_XXXXXXXX"
}
```

**广播：**
```json
{
  "type": "group_member_joined",
  "groupId": "grp_XXXXXXXX",
  "userId": "string",
  "nickname": "string"
}
```

**错误响应：**
| code | 含义 |
|------|------|
| `NOT_JOINED` | 未加入聊天室 |
| `GROUP_NOT_FOUND` | 群组不存在 |
| `PERMISSION_DENIED` | 被该群封禁 |

---

### 5.3 离开群组 `leave_group`

**请求：**
```json
{
  "type": "leave_group",
  "groupId": "grp_XXXXXXXX"
}
```

**广播：**
```json
{
  "type": "group_member_left",
  "groupId": "grp_XXXXXXXX",
  "userId": "string",
  "nickname": "string"
}
```

---

### 5.4 发送群消息 `send_group_message`

**请求：**
```json
{
  "type": "send_group_message",
  "groupId": "grp_XXXXXXXX",
  "text": "string"
}
```

**广播：**
```json
{
  "type": "group_message",
  "groupId": "grp_XXXXXXXX",
  "userId": "string",
  "nickname": "string",
  "text": "string",
  "ts": 1719000000,
  "level": 1
}
```

**错误响应：**
| code | 含义 |
|------|------|
| `NOT_JOINED` | 未加入聊天室 |
| `GROUP_NOT_FOUND` | 群组不存在 |
| `PERMISSION_DENIED` | 不是群成员 |
| `MESSAGE_TOO_LONG` | 消息超过长度限制 |

---

### 5.5 获取群成员 `get_group_members`

**请求：**
```json
{
  "type": "get_group_members",
  "groupId": "grp_XXXXXXXX"
}
```

**响应（仅发送给本人）：**
```json
{
  "type": "group_members",
  "groupId": "grp_XXXXXXXX",
  "members": [
    {
      "userId": "string",
      "nickname": "string",
      "level": 1,
      "isAdmin": false,
      "isOwner": true
    }
  ]
}
```

---

### 5.6 获取群消息历史 `get_group_message_history`

**请求：**
```json
{
  "type": "get_group_message_history",
  "groupId": "grp_XXXXXXXX",
  "limit": 50            // 可选，默认 50
}
```

**响应（仅发送给本人）：**
```json
{
  "type": "group_message_history",
  "groupId": "grp_XXXXXXXX",
  "messages": [
    {
      "userId": "string",
      "nickname": "string",
      "text": "string",
      "ts": 1719000000,
      "type": "group",
      "fileUrl": "",
      "fileName": "",
      "fileSize": 0,
      "isRecalled": false
    }
  ]
}
```

---

### 5.7 踢出群成员 `kick_user`

**请求：**
```json
{
  "type": "kick_user",
  "targetUserId": "string",
  "groupId": "grp_XXXXXXXX"
}
```

**广播：**
```json
{
  "type": "user_kicked",
  "groupId": "grp_XXXXXXXX",
  "userId": "string",
  "kickedBy": "string"
}
```

**错误响应：**
| code | 含义 |
|------|------|
| `NOT_JOINED` | 未加入聊天室 |
| `GROUP_NOT_FOUND` | 群组不存在 |
| `NOT_GROUP_ADMIN` | 非群管理员 |
| `PERMISSION_DENIED` | 无踢人权限 |

---

### 5.8 封禁群成员 `ban_user`

**请求：**
```json
{
  "type": "ban_user",
  "targetUserId": "string",
  "groupId": "grp_XXXXXXXX"
}
```

**广播：**
```json
{
  "type": "user_banned",
  "groupId": "grp_XXXXXXXX",
  "userId": "string",
  "bannedBy": "string"
}
```

**错误响应：**
| code | 含义 |
|------|------|
| `NOT_JOINED` | 未加入聊天室 |
| `GROUP_NOT_FOUND` | 群组不存在 |
| `NOT_GROUP_ADMIN` | 非群管理员 |
| `PERMISSION_DENIED` | 无封禁权限 |

---

## 六、查询（Query）

### 6.1 获取在线用户 `get_online_users`

**请求：**
```json
{
  "type": "get_online_users"
}
```

**响应（仅发送给本人）：**
```json
{
  "type": "online_users",
  "users": [
    { "userId": "string", "nickname": "string", "level": 1 }
  ]
}
```

---

### 6.2 获取用户信息 `get_user_info`

**请求：**
```json
{
  "type": "get_user_info",
  "targetUserId": "string"
}
```

**响应（仅发送给本人）：**
```json
{
  "type": "user_info",
  "userId": "string",
  "username": "string",
  "nickname": "string",
  "level": 1,
  "online": true,
  "lastActive": 1719000000,
  "permissions": {
    "canSendMessage": true,
    "canSendPrivate": true,
    "canCreateGroup": false,
    "canManageGroup": false,
    "maxMessageLength": 500,
    "maxGroups": 3
  }
}
```

---

### 6.3 获取公聊消息历史 `get_message_history`

**请求：**
```json
{
  "type": "get_message_history",
  "limit": 50,              // 可选，默认 50
  "before": 1719000000      // 可选，获取此时间戳之前的消息（用于分页加载）
}
```

**响应（仅发送给本人）：**
```json
{
  "type": "message_history",
  "messages": [
    {
      "userId": "string",
      "nickname": "string",
      "text": "string",
      "ts": 1719000000,
      "type": "text",
      "fileUrl": "",
      "fileName": "",
      "fileSize": 0,
      "isRecalled": false
    }
  ]
}
```

> **分页加载说明：**  
> - 首次加载：不传 `before` 参数，获取最新消息  
> - 加载更多：传入当前最早消息的 `ts` 作为 `before` 参数  
> - 响应为空数组表示没有更多历史消息

---

### 6.4 获取私聊消息历史 `get_private_message_history`

**请求：**
```json
{
  "type": "get_private_message_history",
  "otherUserId": "string", // 对方用户 ID
  "limit": 50              // 可选，默认 50
}
```

**响应（仅发送给本人）：**
```json
{
  "type": "private_message_history",
  "otherUserId": "string",
  "messages": [
    {
      "userId": "string",
      "nickname": "string",
      "text": "string",
      "ts": 1719000000,
      "type": "text",
      "fileUrl": "",
      "fileName": "",
      "fileSize": 0,
      "isRecalled": false,
      "receiverId": "string"
    }
  ]
}
```

---

## 七、错误码汇总

| code | HTTP 近似 | 含义 |
|------|-----------|------|
| `NICKNAME_TAKEN` | 409 | 昵称已被占用 |
| `RATE_LIMITED` | 429 | 连接数已达上限 |
| `MESSAGE_TOO_LONG` | 400 | 消息超过长度限制 |
| `INVALID_FORMAT` | 400 | 消息格式错误或字段为空 |
| `NOT_JOINED` | 401 | 未加入聊天室（需先 join 或 login） |
| `USERNAME_EXISTS` | 409 | 用户名已存在 |
| `INVALID_CREDENTIALS` | 401 | 用户名或密码错误 |
| `USER_NOT_FOUND` | 404 | 用户不存在 |
| `ALREADY_LOGGED_IN` | 409 | 用户已在线 |
| `GROUP_NOT_FOUND` | 404 | 群组不存在 |
| `NOT_GROUP_ADMIN` | 403 | 不是群管理员 |
| `PERMISSION_DENIED` | 403 | 权限不足 |

**错误响应格式：**
```json
{
  "type": "error",
  "code": "ERROR_CODE",
  "message": "中文错误描述"
}
```

---

## 八、用户等级与权限

| 等级 | level 值 | 私聊 | 创建群 | 踢人 | 封禁 | 消息长度上限 | 群数量上限 |
|------|----------|------|--------|------|------|-------------|-----------|
| Guest | 0 | ✗ | ✗ | ✗ | ✗ | 200 | 0 |
| Member | 1 | ✓ | ✗ | ✗ | ✗ | 500 | 3 |
| VIP | 2 | ✓ | ✗ | ✗ | ✗ | 1000 | 10 |
| Moderator | 3 | ✓ | ✗ | ✓ | ✗ | 2000 | 20 |
| Admin | 4 | ✓ | ✗ | ✓ | ✓ | 5000 | 50 |
| Owner | 5 | ✓ | ✓ | ✓ | ✓ | 10000 | 无限制 |

---

## 九、典型交互流程

### 9.1 公聊流程

```
客户端                          服务端
  │                               │
  ├─ send {type:"login", ...} ──→ │
  │←── {type:"logged_in", ...} ──┤
  ├─ send {type:"join", ...} ───→ │
  │←── {type:"joined", ...} ─────┤
  │←── {type:"online_users",...}─┤  (广播)
  ├─ send {type:"send", ...} ───→ │
  │←── {type:"message", ...} ────┤  (广播给所有人)
  │                               │
```

### 9.2 私聊流程

```
客户端A                         服务端                        客户端B
  │                               │                            │
  ├─ send {type:"private_message",│                            │
  │    receiverId:"B", text:...}→│                            │
  │←── {type:"private_message",}─┤──→ {type:"private_message"}│
  │                               │                            │
```

### 9.3 群聊流程

```
客户端A                         服务端                        客户端B
  │                               │                            │
  ├─ send {type:"create_group"}─→│                            │
  │←── {type:"group_created"} ───┤                            │
  │                               │←── {type:"join_group"} ───┤
  │←── {type:"group_member_joined}│──→ {type:"group_member_} │
  │                               │                            │
  ├─ send {type:"send_group_message",│                         │
  │    groupId:"grp_x", text:...}→│                           │
  │←── {type:"group_message"} ───┤──→ {type:"group_message"}─┤
  │                               │                            │
```

### 9.4 消息编辑流程

```
客户端                          服务端
  │                               │
  ├─ send {type:"edit_message",  │
  │    messageId:123,             │
  │    text:"new text"} ────────→│
  │←── {type:"message_edited",   │
  │     messageId:123,            │
  │     text:"new text"} ────────┤  (广播)
  │                               │
```

### 9.5 消息撤回流程

```
客户端                          服务端
  │                               │
  ├─ send {type:"recall_message",│
  │    messageId:123} ───────────→│
  │←── {type:"message_recalled", │
  │     messageId:123} ──────────┤  (广播)
  │                               │
```

### 9.6 分页加载流程

```
客户端                          服务端
  │                               │
  ├─ send {type:"get_message_   │
  │    history", limit:20} ─────→│
  │←── {type:"message_history",  │
  │     messages:[...]} ─────────┤
  │                               │
  ├─ send {type:"get_message_   │
  │    history", limit:20,       │
  │    before:oldest_ts} ───────→│
  │←── {type:"message_history",  │
  │     messages:[...]} ─────────┤
  │                               │
```

---

## 十、技术实现说明

### 10.1 消息分发机制

服务端使用 **FNV-1a 编译期哈希 + switch 跳转表** 进行消息分发，替代传统的 `std::unordered_map<std::string, std::function>` 方案：

- **零堆分配**：消除了 `std::function` 的堆分配开销
- **编译期优化**：哈希计算在编译期完成，运行时仅需一次哈希比较
- **跳转表优化**：编译器可将 switch 优化为跳转表，O(1) 分发

```cpp
// 编译期哈希函数
[[nodiscard]] static constexpr std::uint64_t hash_string(std::string_view sv) noexcept {
    std::uint64_t hash = 14695981039346656037ULL; // FNV-1a offset basis
    for (char c : sv) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ULL; // FNV-1a prime
    }
    return hash;
}

// 消息分发
switch (hash_string(type)) {
    case hash_string("register"): handle_register(session, json); break;
    case hash_string("login"):    handle_login(session, json); break;
    // ...
}
```

### 10.2 数据库优化

- **SoA 布局**：`MessageRecord` 热字段（id, timestamp, sender_id, text）前置，冷字段（file_url, read_by）后置
- **LRU 缓存**：用户信息和消息历史使用近似 LRU 缓存，读操作使用 shared_lock 实现并发
- **WAL 模式**：SQLite 启用 WAL 模式提升并发写入性能

### 10.3 内存布局优化

- **`final` 标记**：`Database`、`ChatRoom`、`SessionManager`、`SqliteStmt` 均标记为 `final`，启用去虚拟化优化
- **字段重排**：结构体字段按访问频率和对齐要求排列，减少 padding
- **`constexpr` 函数**：`UserPermissions::for_level`、错误码转换等函数标记为 `constexpr`

### 10.4 连接管理

- **最大连接数**：默认 5000，可通过配置文件调整
- **优雅关闭**：支持 SIGINT/SIGTERM 信号处理
- **工作线程**：默认自动检测硬件并发数

---

## 十一、消息类型说明

| messageType | 说明 | 额外字段 |
|-------------|------|----------|
| `text` | 文本消息 | 无 |
| `file` | 文件消息 | `fileUrl`, `fileName`, `fileSize` |
| `image` | 图片消息 | `fileUrl`, `fileName`, `fileSize` |
| `group` | 群消息 | 无 |

---

## 十二、健康检查与系统配置 (v2.3)

### 服务器健康检查

直接发起 WebSocket 连接即可检测服务器状态。连接成功说明服务正常运行。

### 配置参数说明

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `host` | string | `"0.0.0.0"` | 监听地址 |
| `port` | uint16_t | `8080` | 监听端口 |
| `max_connections` | size_t | `5000` | 最大并发连接数 |
| `message_max_length` | size_t | `500` | 单条消息最大字符数 |
| `log_level` | string | `"info"` | 日志级别 (trace/debug/info/warn/error) |
| `log_file` | string | `"logs/im-chat.log"` | 日志文件路径 |
| `db_path` | string | `"imchat.db"` | 数据库文件路径 |
| `user_cache_size` | size_t | `1000` | 用户缓存容量 |
| `message_cache_size` | size_t | `100` | 消息缓存容量 |
| `worker_threads` | size_t | `0` | 工作线程数 (0=自动=CPU核心数) |

### v2.3 技术架构更新

**编译优化**:
- GCC: `-O3 -march=native` — 启用 CPU 特定指令集和自动向量化
- LTO (链接时优化) 默认启用
- spdlog 编译库模式 (构建速度更快)

**数据库改进**:
- 迁移系统带版本号追踪 (schema_version 表)
- 已读回执改为关系表 `message_reads` (替代 JSON 字符串解析)
- 消息 ID 直接索引查询 (O(1) 替代 O(n) 扫描)
- 群组数据持久化到数据库 (不再纯内存)

**信号处理**:
- 使用 `boost::asio::signal_set` 异步信号处理 (替代全局指针 + C 信号处理器)

**WebSocket**:
- 应用层 ping/pong 管理，更快检测半开连接
- Beast 自动 pong 响应
---

## 十二、聊天ID格式

| 聊天类型 | chatId 格式 | 示例 |
|----------|-------------|------|
| 公聊 | `public` | `public` |
| 私聊 | `private_{userId}` | `private_u_12345678` |
| 群聊 | `group_{groupId}` | `group_grp_12345678` |
