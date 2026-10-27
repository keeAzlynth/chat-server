# 前后端接口映射

## WebSocket 消息格式

全部消息为 JSON，`type` 字段区分消息类型。

### 1. 消息结构 (Message)

| 前端 `ChatMessage` | 后端 `MessageRecord` | JSON key | 类型 | 说明 |
|---------------------|----------------------|----------|------|------|
| `id` | `id` | `id` / `messageId` | String→Long | 前端用字符串拼接 `"userId\|timestamp"`，后端用 int64 |
| `userId` | `sender_id` | `userId` / `sender_id` | String | 发送者ID |
| `nickname` | `sender_nickname` | `nickname` | String | 发送者昵称 |
| `text` | `text` | `text` | String | 消息内容 |
| `timestampSeconds` | `timestamp` | `ts` / `timestamp` | Long | Unix 秒 |
| `isPrivate` | — (receiver_id 判断) | — | Boolean | 前端推导：`receiverId.isNotEmpty()` |
| `receiverId` | `receiver_id` | `receiverId` | String | 私聊接收者 |
| `groupId` | `receiver_id` | `groupId` | String | 群聊ID（复用 receiver_id） |
| `isRecalled` | `is_recalled` | `isRecalled` | Boolean | 是否已撤回 |
| `fileUrl` | `file_url` | `fileUrl` | String | 文件URL |
| `fileName` | `file_name` | `fileName` | String | 文件名称 |
| `fileSize` | `file_size` | `fileSize` | Long | 文件字节数 |
| `readBy` | `read_by` | `readBy` | JSON Array→String | 前端解析为 List，后端存 JSON string |
| `isMine` | — | — | Boolean | **客户端推导**，不传输 |
| `deliveryStatus` | — | — | Enum | **客户端推导**，不传输 |
| `reactions` | — | — | Map | **客户端推导**，不传输 |

### 2. Event Types (事件类型)

| type 值 | 方向 | 前端处理 `IncomingEvent` | 说明 |
|---------|------|--------------------------|------|
| `login_success` | S→C | `LoggedIn` | 含 sessionToken, userId |
| `register_success` | S→C | `LoggedIn` | 同上 |
| `joined` | S→C | `Joined` | 加入聊天室成功 |
| `message` | S→C | `Message` | 公聊消息 |
| `private_message` | S→C | `PrivateMessage` | 私聊消息 |
| `group_message` | S→C | `GroupMessage` | 群聊消息 |
| `typing` | S→C | `Typing` | 正在输入 |
| `online_users` | S→C | `OnlineUsers` | 在线用户列表 |
| `message_history` | S→C | `MessageHistory` | 消息历史 |
| `error` | S→C | `ServerError` | 错误信息 |
| `left` | S→C | `UserLeft` | 用户离开 |

### 3. 请求类型 (Client→Server)

| type 值 | 参数 | 说明 |
|---------|------|------|
| `register` | username, password, nickname | 注册 |
| `login` | username, password | 登录 |
| `resume_session` | token | 会话恢复 |
| `join` | nickname | 加入聊天室 |
| `send` | text | 发送公聊消息 |
| `private_message` | receiverId, text | 发送私聊 |
| `group_message` | groupId, text | 发送群聊消息 |
| `typing` | — | 输入状态 |
| `get_message_history` | limit | 获取消息历史 |
| `recall_message` | messageId | 撤回消息 |
| `edit_message` | messageId, newText | 编辑消息 |
| `delete_message` | messageId | 删除消息 |

### 4. ID 类型说明

> ⚠ **差异**: 后端 message_id 为 `int64`，前端为 `String` (拼接 `"userId|timestamp"`)。
> 服务端返回的 `id` 字段为 int64，前端在 `toChatMessage()` 中将其转为 String。
> 这是历史遗留设计，统一为 int64 需要同时改前后端。

---

## v3 增量（向后兼容）

v3 没有重命名或删除任何既有字段，只是补充。Android 客户端无需改动。

### 5. 服务端新增字段

| 事件 | 字段 | 说明 |
|------|------|------|
| `message` / `private_message` / `group_message` | `messageId` | 落库后的真实自增 ID，客户端可立即用于编辑/撤回 |
| `logged_in` | `resumed` | `resume_session` 成功时为 `true` |
| `online_users` | `count` | 在线人数 |
| `message_history` | `hasMore` | 本页是否取满，用于继续翻页 |
| `group_list` | `groups[]` | 见下 |

`group_list` 元素：

```json
{
  "groupId": "grp_1a2b3c",
  "name": "研发组",
  "ownerId": "u_9f8e",
  "createdAt": 1730000000,
  "memberCount": 12,
  "joined": true,
  "lastMessage": "最近的群消息",
  "lastTs": 1730000123
}
```

### 6. 服务端新增事件

| type | 触发 | 处理建议 |
|------|------|----------|
| `group_list` | 客户端请求 `get_groups` | 渲染群组列表 |
| `pong` | 客户端发送 `ping` | 计算 RTT，可忽略 |
| `user_devices` | `get_devices` | 设备管理页 |

### 7. 客户端新增/别名

| type | 说明 |
|------|------|
| `get_groups` | 拉取群组列表（新增） |
| `group_message` | `send_group_message` 的别名，两种写法都接受 |
| `ping` | 心跳，返回 `pong`（`ts` 原样带回） |

### 8. 错误码

`error.code` 取值：

`NICKNAME_TAKEN`、`RATE_LIMITED`、`MESSAGE_TOO_LONG`、`INVALID_FORMAT`、
`NOT_JOINED`、`USERNAME_EXISTS`、`INVALID_CREDENTIALS`、`USER_NOT_FOUND`、
`ALREADY_LOGGED_IN`、`GROUP_NOT_FOUND`、`NOT_GROUP_ADMIN`、`PERMISSION_DENIED`

### 9. HTTP 接口

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | Web 客户端 |
| GET | `/api/health` | `{status, version, uptimeSeconds}` |
| GET | `/api/stats` | 连接数、在线人数、命令数、收发字节、队列深度 |
| POST | `/api/upload?name=<文件名>` | 请求体为原始字节，返回 `{url, name, size}` |
| GET | `/uploads/<file>` | 下载已上传文件 |

上传返回的 `url` 可直接放进 `send_file` 的 `fileUrl`。服务端只接受
`/uploads/` 开头或 `http(s)://` 的 `fileUrl`。

