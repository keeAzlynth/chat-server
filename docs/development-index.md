# 开发索引

- 需求文件：`F:\chat\IM聊天室课程项目_解耦开发方案.md`
- Android 项目：`F:\chat\android-client`
- Android 入口：`F:\chat\android-client\app\src\main\java\com\course\imchat\MainActivity.kt`
- Android UI：`F:\chat\android-client\app\src\main\java\com\course\imchat\ui\ChatApp.kt`
- Android 数据模型：`F:\chat\android-client\app\src\main\java\com\course\imchat\ChatModels.kt`
- Android ViewModel：`F:\chat\android-client\app\src\main\java\com\course\imchat\ChatViewModel.kt`
- Android WebSocket：`F:\chat\android-client\app\src\main\java\com\course\imchat\data\WebSocketClient.kt`
- Android 序列化：`F:\chat\android-client\app\src\main\java\com\course\imchat\data\MessageSerializer.kt`
- Android 事件定义：`F:\chat\android-client\app\src\main\java\com\course\imchat\data\IncomingEvent.kt`
- Android Repository：`F:\chat\android-client\app\src\main\java\com\course\imchat\data\MessageRepository.kt`
- Android 主题：`F:\chat\android-client\app\src\main\java\com\course\imchat\ui\theme\Theme.kt`
- Android 构建：`F:\chat\android-client\app\build.gradle.kts`
- 后端入口：`F:\chat\src\main.cpp`
- 后端逻辑：`F:\chat\src\chat_room.cpp`
- 后端会话管理：`F:\chat\src\session_manager.cpp`
- 后端数据库：`F:\chat\src\database.cpp`
- 后端头文件：`F:\chat\include\imchat\`
- 构建文件：`F:\chat\CMakeLists.txt`
- 配置文件：`F:\chat\config\config.json`
- 启动说明：`F:\chat\README.md`
- 环境路径：`F:\chat\docs\environment-paths.md`
- 详细流程：`F:\chat\docs\development-process.md`

## 快速环境

### Android

```powershell
cd F:\chat\android-client
$env:JAVA_HOME = "F:\Android_Studio\jbr"
$env:Path = "$env:JAVA_HOME\bin;$env:Path"
```

### C++

```powershell
cd F:\chat
$env:Path = "F:\MSYS2\mingw64\bin;F:\MSYS2\usr\bin;$env:Path"
```

## 构建

### Android

```powershell
.\gradlew.bat assembleDebug
```

APK：`F:\chat\android-client\app\build\outputs\apk\debug\app-debug.apk`

### C++

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## 运行

```powershell
.\build\im_chat_server.exe .\config\config.json
```

## WebSocket 地址

- PC：`ws://127.0.0.1:8080`
- Android 模拟器：`ws://10.0.2.2:8080`
