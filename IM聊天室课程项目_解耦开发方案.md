# 工业级 IM 聊天室课程项目 —— 解耦开发方案（最终版）

> 适用场景：课程论文 / 答辩项目
> 团队：C++ 后端 1 人 + Android 前端 1 人 + 组长 1 人（统筹/审批契约变更）
> 核心原则：**契约先行，两条线并行开发，互不阻塞**

---

## 一、整体架构

```
┌──────────────────────────────────────────────────┐
│              Android 客户端                        │
│  ┌─────────────────────────────────────────────┐ │
│  │ UI Layer (Jetpack Compose + Material3)       │ │
│  │  ├── NavHost (Login ↔ Chat)                  │ │
│  │  ├── ChatScreen                              │ │
│  │  │   ├── TopBar（连接状态指示器）              │ │
│  │  │   ├── MessageList（LazyColumn + 动画）     │ │
│  │  │   └── InputBar（TextField + 发送 + typing）│ │
│  │  └── Theme: 深色 + Inter 字体 + 渐变气泡       │ │
│  ├─────────────────────────────────────────────┤ │
│  │ State Layer (ViewModel + StateFlow)          │ │
│  │  ├── ChatUiState (sealed class)              │ │
│  │  └── MessageRepository                       │ │
│  ├─────────────────────────────────────────────┤ │
│  │ Data Layer (OkHttp WebSocket Client)         │ │
│  │  ├── WebSocketClient（封装连接/重连/心跳）     │ │
│  │  ├── MessageSerializer（Gson）                │ │
│  │  └── BuildConfig.WS_URL（环境隔离）            │ │
│  └─────────────────────────────────────────────┘ │
└────────────────────┬─────────────────────────────┘
                      │ ws://（开发）/ wss://（生产)
                      │ JSON over WebSocket
┌────────────────────▼─────────────────────────────┐
│              C++ 后端 (Win11 / MinGW)              │
│  ┌─────────────────────────────────────────────┐ │
│  │ Network Layer (Boost.Beast + Asio)           │ │
│  │  ├── WebSocketSession（per-connection）       │ │
│  │  └── SessionManager（连接池/广播）             │ │
│  ├─────────────────────────────────────────────┤ │
│  │ Logic Layer（纯C++，无网络依赖）               │ │
│  │  ├── ChatRoom（用户表 + 消息广播）             │ │
│  │  ├── MessageRouter（type → handler）          │ │
│  │  └── ErrorCode（enum class）                  │ │
│  ├─────────────────────────────────────────────┤ │
│  │ Infra Layer                                  │ │
│  │  ├── ConfigLoader（nlohmann/json）            │ │
│  │  ├── Logger（spdlog，async + rotating）       │ │
│  │  └── config.json（端口/限流/日志级别）         │ │
│  └─────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────┘

         ┌──────────────────────────────┐
         │   消息契约（JSON Schema）     │
         │   两人签字确认，变更需组长审批  │
         └──────────────────────────────┘
```

**架构关键决策**（已落定，不再讨论）：

| 决策点 | 选择 | 理由 |
|---|---|---|
| 网络库 | Boost.Beast（非 uWebSockets） | MinGW 下 uWebSockets 的 SSL 链接问题真实存在，Beast 是 Boost 官方一线支持 |
| 序列化 | JSON（非 Protobuf） | 课程项目求快，nlohmann/json ↔ Gson 双端都简单 |
| 开发阶段协议 | `ws://`（非 `wss://`） | 本地开发用 WSS 会引入自签名证书信任问题，增加无意义复杂度；生产环境再升级 |
| Mock 方案 | Node.js + `ws` 库（非 Kotlin 自写 Mock） | 10 分钟搭完，前端开发者不需要懂 Node.js 内部细节，复制脚本即可用 |
| 压测工具 | 自写 C++ 压测客户端（非 wrk） | **wrk 不支持 WebSocket**，只支持 HTTP；自写客户端能精确测 P50/P95/P99，且和后端共享 Beast 代码 |
| MinGW 工具链 | 统一用 MSYS2 的 GCC（非独立 GCC 16） | 避免 ABI 不匹配问题，课程项目场景下 GCC14/16 功能差异可忽略 |
| 模拟器架构 | x86_64（除非主机本身是 ARM） | 有硬件加速，速度快 10 倍 |

---

## 二、消息契约（先于代码，两人必须签字确认）

### 2.1 正常消息

```jsonc
// 客户端 → 服务端：加入聊天室
{ "type": "join", "nickname": "Alice" }

// 服务端 → 客户端：加入成功
{ "type": "joined", "userId": "u_001", "nickname": "Alice" }

// 客户端 → 服务端：发送消息
{ "type": "send", "text": "hello" }

// 服务端 → 所有客户端：广播消息
{ "type": "message", "userId": "u_001", "nickname": "Alice", "text": "hello", "ts": 1719400000 }

// 客户端 ↔ 服务端：正在输入提示
{ "type": "typing" }
```

### 2.2 错误码体系

```jsonc
{ "type": "error", "code": "NICKNAME_TAKEN",   "message": "昵称已被占用" }
{ "type": "error", "code": "RATE_LIMITED",     "message": "发送过于频繁" }
{ "type": "error", "code": "MESSAGE_TOO_LONG", "message": "消息超过500字" }
{ "type": "error", "code": "INVALID_FORMAT",   "message": "消息格式错误" }
```

- 后端用 `enum class ErrorCode` 定义
- Android 端用 `sealed class` 映射，UI 层按类型分发不同提示文案

### 2.3 契约冻结规则

- **字段一旦确认，不允许改名或删除**，只能新增字段（前端必须忽略未知字段）
- 任何变更必须经组长审批，双方需重新确认后才能继续开发

---

## 三、两人独立任务清单

### 3.1 后端开发者（C++ / Win11 / MinGW）

| 阶段 | 产出 | 验证方式 |
|---|---|---|
| 1 | Boost.Beast WSS 服务端骨架，实现 `join` / `send` / `message` | `wscat` 手动连接，按契约收发 |
| 2 | `ChatRoom` 会话状态机，与网络层解耦的纯逻辑类 | GTest 单测，覆盖率 > 70% |
| 3 | 自写 C++ 压测客户端（基于 Boost.Beast） | 输出 QPS / P50 / P95 / P99，并发梯度 10→100→500→1000→2000 |
| 4 | 接入 spdlog（异步 + 轮转日志） + `config.json` 配置加载 | 日志文件正常轮转，端口/限流走配置而非硬编码 |
| 5 | 提供启动 README 给前端 | 前端能照着独立连上 |

**核心纪律：全程不打开 Android Studio。** 所有验证在纯 C++/命令行环境完成。

### 3.2 Android 开发者（Kotlin / Compose）

| 阶段 | 产出 | 验证方式 |
|---|---|---|
| 1 | 跑通 `mock-server.js`（Node.js + `ws`），地址配 `ws://10.0.2.2:8080` | 模拟器能收到 Mock 推送的消息 |
| 2 | LazyColumn 聊天界面 + 深色主题 + Inter 字体 + 渐变气泡 + 动画 | 接在 Mock 上跑通完整 UI 交互 |
| 3 | 已读状态指示器（发送中 / 已发送 ✓ / 已送达 ✓✓）+ "正在输入" 提示 | 基于 Mock 模拟事件验证状态切换 |
| 4 | OkHttp WebSocket 客户端骨架，地址做成 `BuildConfig.WS_URL`（debug/release 隔离） | 切换地址行为不变即通过 |
| 5 | Compose Testing 覆盖发送/接收/连接状态三类场景 | 核心逻辑覆盖率 > 60% |

**核心纪律：全程不依赖后端是否写完。** Mock 就是"假后端"，契约不变则可独立推进到接近完工。

### Mock Server 脚本（直接复制可用）

```javascript
// mock-server.js
const { WebSocketServer } = require('ws');
const wss = new WebSocketServer({ port: 8080 });

wss.on('connection', (ws) => {
  ws.on('message', (data) => {
    const msg = JSON.parse(data);
    if (msg.type === 'join') {
      ws.send(JSON.stringify({ type: 'joined', userId: 'u_' + Date.now(), nickname: msg.nickname }));
      setTimeout(() => {
        ws.send(JSON.stringify({ type: 'message', userId: 'u_bot', nickname: 'Bot', text: '欢迎加入', ts: Date.now() / 1000 }));
      }, 3000);
    }
    if (msg.type === 'send') {
      ws.send(JSON.stringify({ type: 'message', userId: 'u_self', nickname: 'Me', text: msg.text, ts: Date.now() / 1000 }));
    }
  });
});
```

```bash
npm install ws
node mock-server.js
```

---

## 四、环境搭建坑点清单

| 坑点 | 问题 | 解决方案 |
|---|---|---|
| MSYS2 与 Boost ABI | 独立装的 GCC16 与 MSYS2 预编译 Boost 可能 ABI 不匹配 | **统一用 MSYS2 自带 GCC**（`pacman -S mingw-w64-x86_64-gcc`），放弃独立 GCC16，课程项目场景下功能差异可忽略 |
| MSYS2 PATH 冲突 | MSYS2 的 git 可能与 Android Studio 自带 Git 冲突 | MSYS2 PATH 放系统 PATH 最后；Android Studio 的 Git 路径在 `Settings → Version Control → Git` 手动指定 |
| 模拟器架构 | ARM 模拟器在 x86 主机上靠软件翻译，卡顿严重 | 选 **x86_64** 模拟器（有 HAXM/WHPX 硬件加速），除非主机本身是 ARM（如 Surface Pro X）|
| WSS 本地证书 | Android 默认不信任自签名证书 | 开发阶段用 `ws://`；如需演示 WSS，用 **mkcert** 生成本地可信证书，放到最后做 |

### MSYS2 依赖安装清单

```bash
pacman -S mingw-w64-x86_64-gcc
pacman -S mingw-w64-x86_64-boost
pacman -S mingw-w64-x86_64-nlohmann-json
pacman -S mingw-w64-x86_64-openssl
pacman -S mingw-w64-x86_64-gtest
pacman -S mingw-w64-x86_64-spdlog
```

---

## 五、补充架构层（原方案缺失，现已纳入）

| 补充层 | 后端方案 | Android 方案 |
|---|---|---|
| **日志** | spdlog（异步 + 轮转文件） | Timber（Release 自动移除 DEBUG 日志） |
| **配置管理** | `config.json` + nlohmann/json 解析 | `BuildConfig` + `local.properties` 环境隔离 |
| **错误处理** | `enum class ErrorCode` | `sealed class` 映射 + 按类型分发 UI 提示 |

**后端配置示例** `config.json`：
```json
{
  "port": 8080,
  "max_connections": 5000,
  "heartbeat_interval_sec": 30,
  "log_level": "debug"
}
```

**Android 环境隔离** `build.gradle.kts`：
```kotlin
android {
    buildTypes {
        debug {
            buildConfigField("String", "WS_URL", "\"ws://10.0.2.2:8080\"")
        }
        release {
            buildConfigField("String", "WS_URL", "\"wss://your-server.com\"")
        }
    }
}
```

---

## 六、UI 细节加分项

| 项目 | 说明 | 实现成本 |
|---|---|---|
| 字体换 Inter | 用 `ui-text-google-fonts`，中文走系统 Noto Sans CJK | 低（几行配置） |
| 已读状态 | 发送中 → ✓已发送 → ✓✓已送达，依赖后端广播带 `messageId` | 中 |
| "正在输入"提示 | 客户端每 1.5s 发 `typing`，后端广播，3s 无后续清除 | 低，但演示效果好 |
| 深色 + 渐变气泡 | 自己消息渐变蓝紫（`#4F46E5 → #7C3AED`），对方纯色浅灰卡片 | 低 |
| 连接状态指示器 | 顶部细条，断开红色"重连中..."，连上后收起 | 低，投入产出比最高 |

---

## 七、执行计划（按两人各投入约 20 小时估算）

| 阶段 | 后端 | Android | 组长检查点 |
|---|---|---|---|
| D1：契约确认 | 签字 | 签字 | ✅ 确认 JSON Schema，冻结字段 |
| D2-D3：骨架 | Boost.Beast WSS 骨架，wscat 验证 | Node.js Mock + OkHttp 连通 | 互不依赖，各自跑通 |
| D4-D5：核心逻辑 | ChatRoom 状态机 + GTest 单测 | LazyColumn + 气泡动画 + 状态指示器 | 检查单测覆盖率 |
| D6：假集成 | 启动服务，等前端连 | Mock 地址 → `ws://localhost:8080` | ✅ 验证契约一致性 |
| D7-D8：完善 | 压测客户端 + 数据采集 | typing 指示 + 已读状态 + 主题打磨 | 各自整理论文素材 |
| D9：最终集成 | 性能优化（如有瓶颈） | 真实地址联调 | ✅ 全流程演示 |
| D10：论文素材 | 压测数据表 + 火焰图 | UI 录屏 + 性能剖析截图 | 合并进论文 |

---

## 八、代码质量工具清单

| 层面 | 后端 | Android | 说明 |
|---|---|---|---|
| 格式化 | clang-format | ktlint | 提交前自动格式化 |
| 静态分析 | clang-tidy | Detekt + Android Lint | CI 中强制跑 |
| 单元测试 | Google Test | JUnit5 + Mockito | 后端覆盖率 > 70%，前端核心逻辑 > 60% |
| UI 测试 | — | Compose Testing | 至少覆盖发送/接收/连接状态 |
| 性能测试 | 自写 C++ 压测客户端 | Macrobenchmark（可选） | 后端是论文重点 |
| CI | GitHub Actions | GitHub Actions | 同一 repo，两个 workflow |

---

## 九、组长（你）的三个管控点

1. **契约变更必须经你审批**，任何一方想改字段先过一遍，确认不会破坏对方已完成的工作
2. **提前预演集成**：建议两人各完成约 70% 时就做一次"假集成"——后端跑起来，前端把 Mock 地址换成 `ws://localhost:8080`，尽早暴露契约理解偏差
3. **压测数据与 UI 素材分别整理**：论文"性能"章节和"颜值"章节素材分别来自两人独立产出，互不依赖，可提前各自整理

---

## 十、一句话总结

架构方向（契约先行 + Beast 替代 uWebSockets + 前后端解耦）完全正确，三处实操修正点是：**Mock 换 Node.js（不用 Kotlin 自写）、压测换自写 C++ 客户端（wrk 不支持 WebSocket）、开发阶段用 `ws://` 不用 `wss://`**。其余（日志、配置、错误码、UI 细节）是锦上添花。按本计划，两人 10 天内可交付一个"不像作业"的工业级 IM 原型。
