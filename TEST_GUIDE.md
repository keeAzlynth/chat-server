# IM Chat 测试指南

## 前置条件

1. 后端服务已启动（端口8080）
2. ngrok已配置并启动

---

## 一、启动后端

```powershell
cd F:\chat
.\build\im_chat_server.exe .\config\config.json
```

验证：
```powershell
netstat -ano | findstr ":8080" | findstr "LISTENING"
```

---

## 二、启动内网穿透

```powershell
F:\chat\tools\ngrok.exe http 8080
```

查看公网地址：
- 方式1：查看ngrok弹出窗口
- 方式2：浏览器打开 http://127.0.0.1:4040

公网地址格式：`https://xxxx.ngrok-free.app`

---

## 三、Android客户端配置

### 修改连接地址

编辑 `android-client/app/build.gradle.kts`：

```kotlin
// Debug模式使用ngrok地址（wss协议）
buildConfigField("String", "WS_URL", "\"wss://你的ngrok地址\"")
```

> 注意：ngrok免费版地址每次重启会变，需要重新配置

### 重新构建APK

```powershell
cd F:\chat\android-client
$env:JAVA_HOME = "F:\Android_Studio\jbr"
.\gradlew.bat assembleDebug
```

APK位置：`app/build-outputs/apk-debug/app-debug.apk`

---

## 四、测试流程

### 4.1 基础连接测试

1. 打开APP
2. 输入服务器地址（如 `wss://xxxx.ngrok-free.app`）
3. 点击连接
4. 验证：显示"在线"状态

### 4.2 注册/登录测试

1. 点击"注册"创建账号
2. 输入用户名、密码、昵称
3. 验证：注册成功并自动登录

### 4.3 公聊测试

1. 登录后自动进入公共聊天室
2. 发送消息
3. 验证：消息显示在右侧（蓝色气泡）

### 4.4 私聊测试

1. 点击右侧在线用户列表
2. 选择一个用户
3. 发送私聊消息
4. 验证：只有双方能看到消息

### 4.5 群聊测试

1. 点击侧边栏"+"创建群聊
2. 输入群名称
3. 其他用户加入群聊
4. 发送群消息
5. 验证：群成员都能看到消息

### 4.6 Markdown消息测试

发送包含Markdown标记的消息：
```
/md
这是**粗体**
这是*斜体*
这是`代码`
/end
```

验证：消息以Markdown格式渲染

### 4.7 引用消息测试

1. 长按某条消息
2. 选择"引用消息"
3. 输入回复内容
4. 发送
5. 验证：消息上方显示被引用的内容

---

## 五、常用命令

### 查看后端日志
```powershell
Get-Content F:\chat\logs\im-chat.log -Wait
```

### 重启后端
```powershell
# 停止
Get-Process -Name "im_chat_server" | Stop-Process

# 启动
cd F:\chat
.\build\im_chat_server.exe .\config\config.json
```

### 重启ngrok
```powershell
# 停止
Get-Process -Name "ngrok" | Stop-Process

# 启动
F:\chat\tools\ngrok.exe http 8080
```

### 构建前端
```powershell
cd F:\chat\android-client
$env:JAVA_HOME = "F:\Android_Studio\jbr"
.\gradlew.bat assembleDebug
```

---

## 六、故障排除

| 问题 | 解决方案 |
|------|----------|
| 连接失败 | 检查后端是否启动，ngrok是否运行 |
| ngrok地址变了 | 重新查看ngrok窗口，更新build.gradle.kts |
| 消息发不出去 | 检查是否已登录并加入聊天室 |
| 模拟器无法连接 | 使用 `ws://10.0.2.2:8080` |

---

## 七、网络地址对照表

| 场景 | 地址 |
|------|------|
| 本机模拟器 | `ws://10.0.2.2:8080` |
| 本机调试 | `ws://127.0.0.1:8080` |
| 同WiFi真机 | `ws://192.168.x.x:8080` |
| 不同网络(ngrok) | `wss://xxxx.ngrok-free.app` |

---

## 八、Web 客户端（最快验证方式）

不需要 Android 环境，浏览器直接打开：

```
http://127.0.0.1:8081/
```

注册后即可发消息；另开一个浏览器无痕窗口注册第二个账号，可以验证公聊、私聊、
群组、输入状态和已读回执。

自检接口：

```bash
curl http://127.0.0.1:8081/api/health
curl http://127.0.0.1:8081/api/stats
```

---

## 九、自动化端到端测试

`tools/e2e.js` 用 Node 内置 WebSocket 模拟两个客户端，覆盖：

注册 → 加入 → 公聊 → 私聊 → 建群/入群 → 群消息定向投递 → 历史 →
编辑 → 撤回 → 越权拦截 → 限流 → 中文搜索 → 会话恢复 → 多端投递

```bash
# 1. 用独立数据库启动（避免污染 imchat.db）
./build/im_chat_server.exe ./build-verify/test-config.json

# 2. 另一个终端
IM_PORT=18080 node tools/e2e.js
```

> 需要 Node 18+（内置 WebSocket）。全部通过时最后一行输出 `全部通过`。

也可以指定端口测正式实例：

```bash
node tools/e2e.js            # 默认连 ws://127.0.0.1:8080
```
