# IM Chat 项目构建指南

本文档详细说明如何编译构建 IM Chat 项目的后端（C++）和前端（Android）。

---

## 目录

- [环境要求](#环境要求)
- [后端构建（C++）](#后端构建c)
- [前端构建（Android）](#前端构建android)
- [常见问题](#常见问题)

---

## 环境要求

### 后端（C++）

| 工具 | 版本要求 | 说明 |
|------|----------|------|
| MSYS2 | 最新版 | 提供 MINGW64 工具链 |
| CMake | ≥ 3.25 | 构建系统 |
| Ninja | 最新版 | 构建工具 |
| Boost | ≥ 1.70 | 网络库 |
| nlohmann/json | 最新版 | JSON 解析 |
| spdlog | 最新版 | 日志库 |
| SQLite3 | 最新版 | 数据库 |
| libsodium | 最新版 | 加密库 |

### 前端（Android）

| 工具 | 版本要求 | 说明 |
|------|----------|------|
| Android Studio | 最新版 | 开发 IDE |
| JDK | 17 | Java 开发工具包 |
| Android SDK | API 36 | Android 开发包 |

---

## 后端构建（C++）

### 1. 安装 MSYS2

从 https://www.msys2.org/ 下载并安装 MSYS2。

### 2. 安装依赖

打开 **MSYS2 MINGW64** 终端，执行以下命令：

```bash
# 更新包管理器
pacman -Syu

# 安装工具链和依赖
pacman -S mingw-w64-x86_64-toolchain
pacman -S mingw-w64-x86_64-cmake
pacman -S mingw-w64-x86_64-ninja
pacman -S mingw-w64-x86_64-boost
pacman -S mingw-w64-x86_64-nlohmann-json
pacman -S mingw-w64-x86_64-spdlog
pacman -S mingw-w64-x86_64-sqlite3
pacman -S mingw-w64-x86_64-libsodium
```

### 3. 配置环境变量（PowerShell）

```powershell
$env:Path = "F:\MSYS2\mingw64\bin;F:\MSYS2\usr\bin;$env:Path"
```

> **注意**：如果你的 MSYS2 安装在其他路径，请相应修改。

### 4. 生成构建文件

> **v2.3 更新**: CMake 现在自动查找 SQLite3 和 libsodium，不再需要硬编码 MSYS2 路径。
> 如遇到找不到库的情况，MSYS2 用户请确保 `C:/msys64/mingw64/bin` 或 `F:/MSYS2/mingw64/bin` 在 PATH 中。

```powershell
cd F:\chat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

### 5. 编译

```powershell
cmake --build build
```

### 6. 编译产物

编译完成后，可执行文件位于：

```
F:\chat\build\im_chat_server.exe      # 服务器主程序
F:\chat\build\stress_test.exe          # 压力测试工具
```

### 7. 启动服务器

```powershell
.\build\im_chat_server.exe .\config\config.json
```

服务器默认监听地址：`ws://127.0.0.1:8080`

---

## 前端构建（Android）

### 1. 安装 Android Studio

从 https://developer.android.com/studio 下载并安装 Android Studio。

### 2. 配置 JDK

Android Studio 自带 JBR（JetBrains Runtime），路径通常为：

```
F:\Android_Studio\jbr
```

### 3. 构建 Debug APK

**方式一：使用命令行（PowerShell）**

```powershell
cd F:\chat\android-client

# 设置 JAVA_HOME（如果需要）
$env:JAVA_HOME = "F:\Android_Studio\jbr"
$env:Path = "$env:JAVA_HOME\bin;$env:Path"

# 构建 Debug APK
.\gradlew.bat assembleDebug
```

**方式二：使用 Android Studio**

1. 打开 Android Studio
2. 选择 `File -> Open`，选择 `F:\chat\android-client` 目录
3. 等待 Gradle 同步完成
4. 点击 `Build -> Build Bundle(s) / APK(s) -> Build APK(s)`

### 4. APK 产物

构建完成后，APK 位于：

```
F:\chat\android-client\app\build\outputs\apk\debug\app-debug.apk
```

### 5. 安装到设备

```powershell
# 使用 adb 安装
adb install F:\chat\android-client\app\build\outputs\apk\debug\app-debug.apk
```

---

## 运行说明

### 网络地址配置

| 场景 | WebSocket 地址 |
|------|----------------|
| 本机调试（后端 + 模拟器同一台机器） | `ws://127.0.0.1:8080` |
| Android 模拟器访问宿主机 | `ws://10.0.2.2:8080` |
| 真机访问同一局域网的服务器 | `ws://<服务器IP>:8080` |

### 配置文件

服务器配置文件位于 `F:\chat\config\config.json`，可修改监听端口等参数。

---

## 常见问题

### Q1: CMake 报错找不到依赖包

**原因**：MSYS2 依赖未安装或环境变量未配置。

**解决**：
1. 确认已在 MSYS2 MINGW64 终端中安装所有依赖
2. 确认已将 `F:\MSYS2\mingw64\bin` 添加到 `PATH`

### Q2: Gradle 构建失败，提示 JDK 版本不兼容

**原因**：项目需要 JDK 17。

**解决**：
```powershell
$env:JAVA_HOME = "F:\Android_Studio\jbr"
$env:Path = "$env:JAVA_HOME\bin;$env:Path"
```

### Q3: Android 模拟器无法连接后端

**原因**：模拟器使用 `10.0.2.2` 访问宿主机的 `localhost`。

**解决**：
- Debug 构建默认使用 `ws://127.0.0.1:8080`
- 如需修改，编辑 `android-client/app/build.gradle.kts` 中的 `buildConfigField`

### Q4: 编译时提示 `libsodium` 找不到

**解决**：
```bash
# 在 MSYS2 MINGW64 终端中执行
pacman -S mingw-w64-x86_64-libsodium
```

### Q5: Windows 下 Ninja 找不到

**解决**：
```powershell
# 确认 Ninja 已安装
where.exe ninja

# 如果没有，通过 MSYS2 安装
pacman -S mingw-w64-x86_64-ninja
```

---

## 快速构建脚本

### 后端一键构建（PowerShell）

```powershell
$env:Path = "F:\MSYS2\mingw64\bin;F:\MSYS2\usr\bin;$env:Path"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
Write-Host "构建完成！可执行文件: F:\chat\build\im_chat_server.exe"
```

### 前端一键构建（PowerShell）

```powershell
cd F:\chat\android-client
$env:JAVA_HOME = "F:\Android_Studio\jbr"
$env:Path = "$env:JAVA_HOME\bin;$env:Path"
.\gradlew.bat assembleDebug
Write-Host "构建完成！APK: F:\chat\android-client\app\build\outputs\apk\debug\app-debug.apk"
```

---

## 项目结构

```
F:\chat\
├── CMakeLists.txt          # C++ 后端构建配置
├── config/
│   └── config.json         # 服务器配置
├── include/
│   └── imchat/             # C++ 头文件
├── src/                    # C++ 源代码
├── tools/
│   └── stress_test.cpp     # 压力测试工具
├── build/                  # 编译输出目录
└── android-client/         # Android 前端
    ├── app/
    │   └── build.gradle.kts
    ├── build.gradle.kts
    └── gradlew.bat
```
