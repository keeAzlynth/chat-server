# IM Chat Server

C++ WebSocket IM 聊天室后端服务。

## 功能

- WebSocket 服务端，默认监听 `ws://0.0.0.0:8080`
- 支持客户端 `join`、`send`、`typing` 消息
- SQLite3 持久化（用户、消息、已读状态）
- libsodium 密码哈希
- spdlog 轮转日志

## 依赖

| 库 | 用途 |
|------|------|
| Boost.Beast | WebSocket 网络 |
| nlohmann/json | JSON 解析 |
| spdlog | 日志 |
| SQLite3 | 数据库 |
| libsodium | 加密 |

## 构建

### 1. 安装依赖（MSYS2）

```bash
pacman -S mingw-w64-x86_64-toolchain
pacman -S mingw-w64-x86_64-cmake
pacman -S mingw-w64-x86_64-ninja
pacman -S mingw-w64-x86_64-boost
pacman -S mingw-w64-x86_64-nlohmann-json
pacman -S mingw-w64-x86_64-spdlog
pacman -S mingw-w64-x86_64-sqlite3
pacman -S mingw-w64-x86_64-libsodium
```

### 2. 配置 PATH

将 MinGW64 的 bin 目录加入 PATH：

```bash
# 替换为你的 MSYS2 安装路径
export PATH="/c/msys64/mingw64/bin:$PATH"
```

### 3. 编译

```bash
bash build.sh Debug
```

或手动：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## 启动

```bash
bash run.sh
```

或手动：

```bash
./build/im_chat_server.exe ./config/config.json
```

## 连接地址

| 场景 | WebSocket 地址 |
|------|----------------|
| 本机调试 | `ws://127.0.0.1:8080` |
| Android 模拟器访问宿主机 | `ws://10.0.2.2:8080` |
| 真机访问局域网服务器 | `ws://<服务器IP>:8080` |

## 配置

配置文件：`config/config.json`，可修改监听地址、端口、日志级别等。
