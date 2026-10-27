# 架构与重构说明

本文记录 v3 重构后的运行模型，以及每一项改动针对的具体问题。

## 1. 线程模型

```
                 ┌──────────────────────────────┐
   TCP accept ──▶│  io_context（N 个 I/O 线程） │
                 │  每连接一个 strand            │
                 └──────────────┬───────────────┘
                                │ 解析 JSON（轻量）
                                ▼
                 ┌──────────────────────────────┐
                 │  ShardedExecutor（M 个分片）  │
                 │  shard = hash(connection_id) │
                 └──────────────┬───────────────┘
                                │ 业务逻辑 + SQLite（阻塞）
                                ▼
                 ┌──────────────────────────────┐
                 │  ChatRoom（读写锁保护内存态） │
                 │  Database（WAL + 语句缓存）   │
                 └──────────────┬───────────────┘
                                │ session->send()（线程安全）
                                ▼
                       回到 socket 的 strand 写出
```

**为什么必须分两层。** 旧实现里 `on_read → handle_message → save_message` 全部在
Reactor 线程上同步执行。SQLite 写一次是几十到几百微秒，登录时 Argon2 校验是几十
毫秒——这段时间里该 I/O 线程完全无法处理其他连接的读写，连接一多就出现"卡顿"。

**为什么还要分片而不是普通线程池。** 普通线程池会让同一个连接的两条命令并行，
"撤回"可能先于"发送"执行。按 `connection_id` 哈希到固定分片，既保证同连接 FIFO，
又让不同连接真正并行。

## 2. 一个问题一张表

| # | 原实现 | 影响 | 现在 |
|---|--------|------|------|
| 1 | 业务与 SQLite 都在 I/O 线程 | 单次写库阻塞所有连接 | 分片线程池执行，Reactor 只做 I/O |
| 2 | `ChatRoom` 一把 `unique_lock` 贯穿全函数，且锁内做数据库 I/O | 全局串行，吞吐被 DB 延迟放大 | 锁只保护内存态；先取快照，释放后再查库 |
| 3 | `users_by_id_[user_id]` 只存一个 `connection_id` | 第二台设备登录会顶掉第一台；任一设备断线会把账号标记离线 | `User.connections` 是集合，全部设备断开才离线 |
| 4 | `send_to_user` 只发第一条连接 | 多端登录时只有一个设备收到私聊 | 遍历该用户全部连接 |
| 5 | 群消息 `broadcast=true` 且无收件人 | 群消息推给所有在线用户，非成员也能收到 | 事件携带 `recipients`，只投递给群成员 |
| 6 | 限流用 `unix_seconds()` 整秒重置，且 `rate_limits_` 永不清除 | 跨秒双倍突发；每个历史连接泄漏一个桶 | 令牌桶按单调时钟连续补充，断连时回收 |
| 7 | `generate_id` 用 `mt19937_64` 取 32 位十六进制，同一函数生成会话令牌 | 几万条消息后可能撞 ID；令牌可预测 | libsodium `randombytes_buf`，令牌 256 位 |
| 8 | `message_to_json` / `error()` 标了 `noexcept` 却构造 `json` | 分配失败直接 `std::terminate` | 去掉错误的 `noexcept` |
| 9 | 未设置 `read_message_max`，未开 `TCP_NODELAY` | 单帧可到 16 MB；Nagle 让每条消息多等最多 40 ms | 帧上限可配（默认 64 KB）；连接开启 `no_delay` |
| 10 | `outgoing_` 队列无上限 | 慢客户端可撑爆内存 | 队列超过 8 MB 主动断开 |
| 11 | `recall/edit/delete` 只判断 `step()==SQLITE_DONE` | 非作者也能"撤回成功"（实际零行更新），权限形同虚设 | 检查 `sqlite3_changes() > 0` |
| 12 | 群组只存在内存，重启即丢 | 重启后 `join_group` 报 GROUP_NOT_FOUND | 启动时从库加载群与成员 |
| 13 | 没有 HTTP 层 | 无法提供网页、健康检查、文件上传 | 新增 `HttpServer`（静态托管 + API） |
| 14 | `std::hash<string>` 的 `constexpr` 包装 | GCC 16 报 `-Winvalid-constexpr` 一堆警告 | 去掉错误的 `constexpr` |
| 15 | 服务端无连接/消息计数 | 无法观测负载 | `/api/stats` 暴露连接数、命令数、流量、队列深度 |

## 3. 事件投递模型

旧接口是 `{payload, target_user_id, broadcast}`，语义模糊——广播时不知道该发给谁，
只能全员推。新模型把"发给谁"写清楚：

```cpp
struct RoomEvent {
    nlohmann::json payload;
    std::vector<std::string> recipients;  // 目标用户（含其全部设备）
    bool broadcast{false};                // 全员
    bool ack{true};                       // 是否回给发起连接
};
```

`SessionManager::dispatch` 的规则很短：

1. `ack` → 先回发起连接；
2. `broadcast` → 除发起连接外全员；
3. 否则逐个 `recipients` → 该用户的全部连接。

因此公聊是 `ack + broadcast`，私聊是 `ack + recipients{对方}`，群聊是
`ack + recipients{其余成员}`，错误只回发起连接。

## 4. 数据库

- `PRAGMA busy_timeout=5000`：没有它，第二个写线程立刻拿到 `SQLITE_BUSY`。
- `journal_mode=WAL` + `synchronous=NORMAL`：写不阻塞读。
- `prepare_cached()`：同一 SQL 只 prepare 一次，`Stmt` 在作用域结束时
  `reset + clear_bindings`。热路径（插入消息）不再重复解析 SQL。
- `save_message()` 返回自增 `messageId`，广播里带上，客户端可立即编辑/撤回。
- 列表查询（`list_groups`）用一条带聚合子查询的 SQL，避免 N+1。
- 检索先用 FTS5，命中为空时回退 `LIKE`：FTS5 默认的 `unicode61` 不切分中日韩，
  中文子串检索会直接漏掉。

## 5. Web 客户端

- 原生 ES + CSS，无构建步骤，`HttpServer` 直接托管 `web/`。
- 会话：`sessionToken` 存 `localStorage`，断线后指数退避重连并自动 `resume_session`。
- 未读数、输入状态、已读回执、置顶/收藏抽屉、搜索结果跳转、图片预览。
- 所有服务端字符串经 `escape` 后写入 DOM。
- 主题跟随 `prefers-color-scheme` 的显式切换（浅色为默认）。

### Markdown 渲染（`web/markdown.js`）

聊天消息里的 Markdown 由自带的轻量渲染器处理，不依赖 CDN，断网也能用：

- 支持围栏代码块（带语言标签和复制按钮）、行内代码、粗体/斜体/删除线、
  标题、有序/无序/任务列表、引用、分隔线、链接、图片、裸链接自动识别。
- **先转义再解析**：所有输入先做 HTML 转义，渲染器只插入自己生成的标签；
  链接协议白名单只允许 `http(s)`、`mailto`、`/uploads/`、`#`，因此
  `javascript:` 与 `onerror=` 这类注入都进不来。
- 不处理下划线斜体（`_x_`），避免把 `foo_bar_baz` 这类标识符拆坏。
- 单行消息走行内渲染，保持紧凑；含代码块/列表的消息切到块级排版并改用浅底气泡。
- 输入框右侧的 👁 可实时预览（120 ms 节流）。

### 渲染性能

聊天列表最容易写成 O(n²)：每来一条消息就重建整张列表，顺带把每条的
Markdown 重解析一遍。现在的做法：

| 手段 | 效果 |
|------|------|
| 解析结果按原文缓存（FIFO，命中不做 LRU 提升） | 3KB 代码块 52 µs → < 0.1 µs |
| 无 Markdown 特征的文本直接跳过全部正则 | 普通消息几乎零成本 |
| 新消息只 `insertAdjacentHTML` 一个节点 | 1000 条在屏时 4 ms → < 0.01 ms |
| 已读回执只加 `.read` class（`::after` 画对勾） | 去掉一次整表重绘 |
| 点击/复制/菜单用事件委托 | 去掉 O(n) 的逐节点绑定 |
| `requestAnimationFrame` 合并同一帧的事件 | 突发消息只渲染一次 |

`web/bench.html` 可以在浏览器里现场对比这几条路径，不需要跑测试框架。

一个反直觉的结论：**LRU 提升反而拖慢缓存**。命中时 `delete + set` 的代价和
解析一条短消息差不多，缓存等于白做；换成 FIFO 淘汰后命中路径才真正变便宜。
这个结论就是基准页跑出来的。

## 6. 仍可继续做的

- 把 `Database` 从"单连接 + 互斥"升级为连接池，让读操作真正并行。
- 定时消息目前只落库，没有投递线程；需要补一个基于 `get_due_scheduled_messages` 的轮询器。
- 文件上传目前存本地磁盘，生产环境应换成对象存储并做类型/内容校验。
- 缺少断线期间的消息补偿（当前靠 `get_message_history` 拉取最近 N 条）。
