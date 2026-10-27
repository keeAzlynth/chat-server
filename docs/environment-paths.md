# 环境路径记录

更新时间：2026-06-26

## 本项目选用

- 编译器方案：MSYS2 MINGW64 GCC
- 选择原因：后端依赖 Boost.Beast、nlohmann-json、spdlog 等 MSYS2 预编译库，统一使用 MSYS2 前缀可以避免 ABI 混用。

## 关键路径

| 项目 | 路径 |
|---|---|
| 工作区 | `F:\chat` |
| Android 项目 | `F:\chat\android-client` |
| Android Studio | `F:\Android_Studio` |
| Android Studio JBR | `F:\Android_Studio\jbr` |
| Android JBR java | `F:\Android_Studio\jbr\bin\java.exe` |
| Android SDK | `C:\Users\DELL\AppData\Local\Android\Sdk` |
| Android SDK Platform | `C:\Users\DELL\AppData\Local\Android\Sdk\platforms\android-36.1` |
| Android build-tools | `C:\Users\DELL\AppData\Local\Android\Sdk\build-tools\36.1.0` / `37.0.0` |
| Android adb | `C:\Users\DELL\AppData\Local\Android\Sdk\platform-tools\adb.exe` |
| Gradle Wrapper | `F:\chat\android-client\gradlew.bat` |
| MSYS2 根目录 | `F:\MSYS2` |
| pacman | `F:\MSYS2\usr\bin\pacman.exe` |
| MINGW64 前缀 | `F:\MSYS2\mingw64` |
| GCC/G++ | `F:\MSYS2\mingw64\bin\g++.exe` |
| CMake | `F:\MSYS2\mingw64\bin\cmake.exe` |
| Ninja | `F:\MSYS2\mingw64\bin\ninja.exe` |
| 头文件目录 | `F:\MSYS2\mingw64\include` |
| 库目录 | `F:\MSYS2\mingw64\lib` |

## 已安装包

### Android / Kotlin

| 项目 | 当前版本 |
|---|---|
| Android Gradle Plugin | `9.0.1` |
| Gradle Wrapper | `9.1.0` |
| Kotlin / Compose Compiler Plugin | `2.4.0` |
| Compose BOM | `2026.06.00` |
| Activity Compose | `1.13.0` |
| Lifecycle | `2.10.0` |
| OkHttp | `5.4.0` |
| Gson | `2.14.0` |

### MSYS2 / C++

| 包 | 当前版本 |
|---|---|
| `mingw-w64-x86_64-gcc` | `16.1.0-5` |
| `mingw-w64-x86_64-boost` | `1.91.0-3` |
| `mingw-w64-x86_64-nlohmann-json` | `3.12.0-1` |
| `mingw-w64-x86_64-openssl` | `3.6.3-1` |
| `mingw-w64-x86_64-gtest` | `1.17.0-1` |
| `mingw-w64-x86_64-spdlog` | `1.17.0-3` |
| `mingw-w64-x86_64-cmake` | `4.3.3-1` |
| `mingw-w64-x86_64-ninja` | `1.13.2-1` |

## 下次快速命令

### Android

```powershell
cd F:\chat\android-client
$env:JAVA_HOME = "F:\Android_Studio\jbr"
$env:Path = "$env:JAVA_HOME\bin;$env:Path"
.\gradlew.bat assembleDebug
```

APK 输出：

```text
F:\chat\android-client\app\build\outputs\apk\debug\app-debug.apk
```

### C++

```powershell
$env:Path = "F:\MSYS2\mingw64\bin;F:\MSYS2\usr\bin;$env:Path"
g++ --version
cmake --version
pacman -Q mingw-w64-x86_64-gcc mingw-w64-x86_64-boost mingw-w64-x86_64-nlohmann-json mingw-w64-x86_64-openssl mingw-w64-x86_64-gtest mingw-w64-x86_64-spdlog
```

## 注意

- Android SDK 当前只有 `android-36.1` 平台，项目用 `compileSdkVersion("android-36.1")` 指向它。这个写法在 AGP 9 可用但提示弃用，等以后安装标准 `android-37` 或 AGP 10 迁移时再调整。
- Gradle Wrapper 使用直连地址 `https://downloads.gradle.org/distributions/gradle-9.1.0-bin.zip`，避开 `services.gradle.org` 重定向下载卡住的问题。
- 当前系统 PATH 里还有 `C:\mingw64\bin\g++.exe`，版本也是 GCC 16.1.0，但它不是本项目选用的 MSYS2 前缀。
- 本项目构建时优先把 `F:\MSYS2\mingw64\bin` 放到 PATH 最前面，防止 CMake 找到错误的编译器或库。
