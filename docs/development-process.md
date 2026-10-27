# 后端开发流程记录

更新时间：2026-06-27

## 目标

先按课程项目文档完成一个不复杂但可继续扩展的 IM 聊天室原型。Android Kotlin 客户端是主要交付物，C++ 后端只作为联调支撑。当前阶段重点是功能骨架，不做完整测试和压测。

## 已阅读的需求来源

- `F:\chat\IM聊天室课程项目_解耦开发方案.md`

关键约束：

- Windows 11 环境
- C++ 后端优先使用 MSYS2 GCC
- 网络层使用 Boost.Beast + Asio
- 消息使用 JSON
- 开发阶段使用 `ws://`
- 先完成 `join`、`send`、`message` 的服务端骨架
- 日志使用 spdlog
- 配置使用 `config.json` + nlohmann/json

## 环境处理

1. 在工作区搜索 Markdown 文件，确认需求文件只有一个：
   - `F:\chat\IM聊天室课程项目_解耦开发方案.md`
2. 检查系统已有工具：
   - PATH 中已有 `C:\mingw64\bin\g++.exe`
   - PATH 中已有 `C:\Program Files\CMake\bin\cmake.exe`
   - PATH 中没有直接暴露 `pacman`
3. 搜索本机 MSYS2 安装位置：
   - 找到 `F:\MSYS2`
   - 找到 `F:\MSYS2\usr\bin\pacman.exe`
4. 按项目文档要求，安装并选用 MSYS2 MINGW64 依赖，避免独立 GCC 与 MSYS2 库混用。

安装命令：

```powershell
& "F:\MSYS2\usr\bin\bash.exe" -lc "pacman -S --needed --noconfirm mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-boost mingw-w64-x86_64-nlohmann-json mingw-w64-x86_64-openssl mingw-w64-x86_64-gtest mingw-w64-x86_64-spdlog"
```

5. 检查 Android 环境：
   - Android Studio：`F:\Android_Studio`
   - JBR：`F:\Android_Studio\jbr`
   - Android SDK：`C:\Users\DELL\AppData\Local\Android\Sdk`
   - SDK Platform：`android-36.1`
6. 当前 PATH 没有全局 Gradle 和 Java，因此 Android 项目使用 Gradle Wrapper，并在 `gradle.properties` 中记录 `org.gradle.java.home=F:/Android_Studio/jbr`。
7. Gradle Wrapper 默认 `services.gradle.org` 下载会卡在 0 字节，已改为 `downloads.gradle.org` 直连。

## 工程结构

```text
F:\chat
├── CMakeLists.txt
├── README.md
├── android-client
│   ├── app
│   │   ├── build.gradle.kts
│   │   └── src/main/java/com/course/imchat
│   │       ├── MainActivity.kt
│   │       ├── ChatViewModel.kt
│   │       ├── ChatModels.kt
│   │       ├── data
│   │       │   ├── MessageRepository.kt
│   │       │   ├── MessageSerializer.kt
│   │       │   ├── IncomingEvent.kt
│   │       │   ├── WebSocketClient.kt
│   │       │   └── EmojiData.kt
│   │       └── ui
│   │           ├── ChatApp.kt
│   │           ├── GlassEffect.kt
│   │           └── theme
│   │               ├── Theme.kt
│   │               └── Type.kt
│   ├── build.gradle.kts
│   ├── gradle.properties
│   ├── gradlew.bat
│   ├── local.properties
│   └── settings.gradle.kts
├── config
│   └── config.json
├── docs
│   ├── development-index.md
│   ├── development-process.md
│   └── environment-paths.md
├── include
│   └── imchat
│       ├── chat_room.hpp
│       ├── config.hpp
│       ├── database.hpp
│       ├── lru_cache.hpp
│       ├── server.hpp
│       ├── session_manager.hpp
│       ├── string_hash.hpp
│       └── websocket_session.hpp
└── src
    ├── chat_room.cpp
    ├── config.cpp
    ├── database.cpp
    ├── main.cpp
    ├── server.cpp
    ├── session_manager.cpp
    └── websocket_session.cpp
```

## 分层设计

## Android 客户端

路径：

- `F:\chat\android-client`

技术栈：

- Kotlin
- Jetpack Compose + Material3
- ViewModel + StateFlow
- OkHttp WebSocket
- Gson JSON
- BuildConfig.WS_URL 环境隔离

### 1. UI 层

文件：

- `android-client/app/src/main/java/com/course/imchat/ui/ChatApp.kt`
- `android-client/app/src/main/java/com/course/imchat/ui/theme/Theme.kt`

职责：

- 昵称登录界面
- 聊天主界面
- 顶部连接状态
- LazyColumn 消息列表
- 输入框和发送按钮
- typing 提示
- Snackbar 错误提示

### 2. State 层

文件：

- `android-client/app/src/main/java/com/course/imchat/ChatViewModel.kt`
- `android-client/app/src/main/java/com/course/imchat/ChatModels.kt`

职责：

- 保存 UI 状态
- 处理加入、重连、输入、发送
- 将 WebSocket 事件归约成 Compose 状态
- 管理本地发送中状态和服务端回显后的已送达状态

### 3. Data 层

文件：

- `android-client/app/src/main/java/com/course/imchat/data/MessageRepository.kt`
- `android-client/app/src/main/java/com/course/imchat/data/WebSocketClient.kt`
- `android-client/app/src/main/java/com/course/imchat/data/MessageSerializer.kt`
- `android-client/app/src/main/java/com/course/imchat/data/IncomingEvent.kt`

职责：

- 用 OkHttp 连接 WebSocket
- 连接成功后发送 `join`
- 发送 `send` / `typing`
- 解析服务端 `joined` / `message` / `typing` / `error`
- 通过 Flow 把事件交给 ViewModel

### Android 构建流程

```powershell
cd F:\chat\android-client
$env:JAVA_HOME = "F:\Android_Studio\jbr"
$env:Path = "$env:JAVA_HOME\bin;$env:Path"
.\gradlew.bat assembleDebug
```

APK：

```text
F:\chat\android-client\app\build\outputs\apk\debug\app-debug.apk
```

### Android 当前注意点

- 本机 SDK 平台是 `android-36.1`，项目用 `compileSdkVersion("android-36.1")`。AGP 9 提示这个方法未来会弃用，但当前可以编译。
- Lifecycle 2.11.0 需要 API 37，本机没装 API 37，因此使用 Lifecycle 2.10.0。
- AGP 9.0.1 要求 Gradle 9.1.0，因此 wrapper 使用 Gradle 9.1.0。

## C++ 后端

### 1. 逻辑层：`ChatRoom`

文件：

- `include/imchat/chat_room.hpp`
- `src/chat_room.cpp`

职责：

- 保存连接 ID 到用户信息的映射
- 保存昵称占用表
- 生成 `u_001` 形式的用户 ID
- 处理加入聊天室、发送消息、typing 事件
- 返回符合契约的 JSON payload
- 生成错误响应

当前支持：

- `join`
- `send`
- `typing`
- 昵称重复校验
- 空昵称、空消息、超长消息校验

### 2. 配置层：`AppConfig`

文件：

- `include/imchat/config.hpp`
- `src/config.cpp`
- `config/config.json`

职责：

- 从 JSON 读取监听地址、端口、最大连接数、消息长度、日志等级、日志文件路径
- 给缺省值，降低配置缺字段时的失败概率

### 3. 会话管理层：`SessionManager`

文件：

- `include/imchat/session_manager.hpp`
- `src/session_manager.cpp`

职责：

- 保存当前 WebSocket session
- 接收网络层传来的原始文本消息
- 解析 JSON 并根据 `type` 路由到 `ChatRoom`
- 对单个 session 发送错误或加入成功响应
- 对所有 session 广播聊天消息和 typing 事件

### 4. 网络层：`Server` / `WebSocketSession`

文件：

- `include/imchat/server.hpp`
- `src/server.cpp`
- `include/imchat/websocket_session.hpp`
- `src/websocket_session.cpp`

职责：

- 使用 Boost.Asio 监听 TCP 端口
- 使用 Boost.Beast 完成 WebSocket 握手
- 异步读取客户端消息
- 异步写出服务端消息
- 断开时通知 `SessionManager` 清理状态

## 消息契约

客户端加入：

```json
{ "type": "join", "nickname": "Alice" }
```

服务端响应：

```json
{ "type": "joined", "userId": "u_001", "nickname": "Alice" }
```

客户端发送消息：

```json
{ "type": "send", "text": "hello" }
```

服务端广播：

```json
{ "type": "message", "userId": "u_001", "nickname": "Alice", "text": "hello", "ts": 1719400000 }
```

客户端发送输入状态：

```json
{ "type": "typing" }
```

服务端广播：

```json
{ "type": "typing", "userId": "u_001", "nickname": "Alice" }
```

错误：

```json
{ "type": "error", "code": "INVALID_FORMAT", "message": "消息格式错误" }
```

## 构建流程

在 PowerShell 中执行：

```powershell
cd F:\chat
$env:Path = "F:\MSYS2\mingw64\bin;F:\MSYS2\usr\bin;$env:Path"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

启动：

```powershell
.\build\im_chat_server.exe .\config\config.json
```

服务地址：

- 本机：`ws://127.0.0.1:8080`
- Android 模拟器访问宿主机：`ws://10.0.2.2:8080`

## 后续建议

1. 优先继续完善 Android 端：连接失败后的自动重连、消息 ID、已读状态细分。
2. 增加 Compose Preview 或截图，方便论文展示 UI。
3. 增加 GTest，覆盖 `ChatRoom` 的纯逻辑。
4. 增加限流逻辑，让 `RATE_LIMITED` 错误码真正生效。
5. 增加压测客户端，输出 QPS、P50、P95、P99。
6. 如需和 Android 联调，先用本地后端或 Node mock 验证同一份 JSON 契约。
7. 最后再考虑 `wss://`，本地开发阶段保持 `ws://`。

## 2026-06-27：UI 优化 + 后端功能适配

### 前端 UI 优化（Telegram 风格）

对 `ChatApp.kt` 进行了全面的布局重构，参考 Telegram 的设计风格：

**布局结构：**
- `ChatContent` 从 `Column` + `imePadding` 改为 `Box` + 输入栏 `align(BottomCenter)` + `navigationBarsPadding`，消息列表不再被输入栏遮挡
- `LazyColumn` 添加 `contentType` 参数优化 item 复用

**消息气泡：**
- 去掉 `AnimatedVisibility` 包裹，改用 `Surface` 实现纯色气泡
- 气泡圆角改为 Telegram 风格（自己消息左上圆角小 4dp，对方消息右上圆角小 4dp）
- 发送气泡：深蓝 `#2B5278`，接收气泡：深灰 `#182533`
- 时间戳嵌入消息文本右侧，已读状态在气泡下方
- 头像改为 `CircleShape`

**输入栏：**
- 使用 `Surface(tonalElevation)` 替代手动画线
- 文本框 `heightIn(min=40, max=120)` 自动扩展
- 圆角 `20dp` 胶囊形状
- 发送按钮改为 `IconButton`，无需时降低透明度

**顶部栏：**
- 使用 `Surface(tonalElevation + shadowElevation)` 替代手动渐变线
- 固定 56dp 高度，圆形头像 + 连接状态绿点

**输入指示器：**
- 使用 `rememberInfiniteTransition` 实现三点脉冲动画

**主题色（Theme.kt）：**
- 深色背景改为 Telegram 蓝灰色调 `#0E1621` / `#17212B`
- 主色调改为 `#5EAFFF`

**其他：**
- 所有 `glassCard` / `glassEffect` 依赖已移除，改用标准 Material3 `Surface`
- Auth/Connect/Join/Settings 页面统一使用简洁 Material3 风格
- 侧边栏使用 `NavigationDrawerItem` 替代手动 Row

### 前端新增 UI 功能

在 `ChatModels.kt`、`ChatViewModel.kt`、`ChatApp.kt`、`MainActivity.kt` 中新增了 10 项功能：

| 功能 | 实现 |
|---|---|
| 消息时间戳 | `ChatMessage.formattedTime()` / `formattedDate()`，气泡内右侧显示 HH:mm |
| 消息已读状态 | `DeliveryStatus.Read`，已读消息用主题色高亮 |
| 图片/文件消息 | `MessageType` 枚举 + `fileUrl`/`fileName`/`fileSize` 字段，输入栏增加附件按钮 |
| 消息搜索 | `ChatUiState.isSearching`/`searchQuery`/`searchResults`，顶部搜索栏 |
| 用户头像 | `OnlineUser.avatarUrl` / `ChatMessage.avatarUrl`，圆形头像显示 |
| 消息撤回 | `ChatMessage.isRecalled`/`recalledAt`，长按消息可撤回 |
| 设置页面 | `SettingsScreen`，显示账号信息、服务器、深色模式开关、退出登录 |
| 主题切换 | `ChatUiState.isDarkMode`，设置页 Switch 控制 |
| 消息通知 | `MainActivity` 回调已接入 |
| 群组管理 | `ChatGroup` 数据模型 + `createGroup`/`selectGroup`/`joinGroup`/`leaveGroup` |

### 后端功能适配

为支持前端新增功能，后端进行了以下扩展：

**数据库（database.hpp / database.cpp）：**
- `messages` 表新增列：`file_url`、`file_name`、`file_size`、`is_recalled`、`recalled_at`、`read_by`
- `users` 表新增列：`avatar_url`
- 自动迁移：`ALTER TABLE ADD COLUMN` 对已有数据库无感升级
- 新增方法：`get_group_messages`、`recall_message`、`mark_message_read`、`update_user_avatar`

**ChatRoom（chat_room.hpp / chat_room.cpp）：**
- 新增 `send_file_message`：发送文件/图片消息，存入 DB 并广播
- 新增 `recall_message`：校验发送者权限后标记撤回，广播 `message_recalled`
- 新增 `mark_read`：将 user_id 追加到消息的 `read_by` JSON 数组
- 新增 `get_group_message_history`：查询群组消息历史
- `get_message_history` / `get_private_message_history` 返回值新增 `fileUrl`/`fileName`/`fileSize`/`isRecalled`

**SessionManager（session_manager.hpp / session_manager.cpp）：**
- 注册 4 个新 handler：`recall_message`、`mark_read`、`send_file`、`get_group_message_history`

### 新增 WebSocket 协议

| type | 方向 | 说明 |
|---|---|---|
| `recall_message` | C→S | `{"type":"recall_message","messageId":123}` |
| `mark_read` | C→S | `{"type":"mark_read","messageId":123}` |
| `send_file` | C→S | `{"type":"send_file","text":"","fileUrl":"...","fileName":"...","fileSize":1024}` |
| `get_group_message_history` | C→S | `{"type":"get_group_message_history","groupId":"grp_xxx","limit":50}` |
| `message_recalled` | S→C | 广播撤回通知 |
| `message_read` | S→C | 回执已读通知 |
| `group_message_history` | S→C | 群组消息历史响应 |

### 前端 Data 层适配

- `MessageSerializer.kt`：新增 8 个序列化方法 + `parse` 支持 3 种新事件 + 历史消息解析含文件/撤回字段
- `IncomingEvent.kt`：新增 `MessageRecalled`/`MessageRead`/`GroupMessageHistory`；`MessageData` 新增 `fileUrl`/`fileName`/`fileSize`/`isRecalled`
- `MessageRepository.kt`：`recallMessage`/`createGroup`/`getGroupMessageHistory`/`joinGroup`/`leaveGroup` 从 TODO 桩改为实际 WebSocket 调用
- `ChatViewModel.kt`：`reduce` 处理新事件；消息历史和实时消息解析含文件/撤回信息

### 编译验证

- 后端：`cmake --build build` 通过（仅有 `-Wmissing-field-initializers` 警告，无错误）
- 前端：`gradlew assembleDebug` 通过（零错误零警告）
