#!/usr/bin/env bash
# IM Chat Server 编译脚本
# 用法: bash build.sh [Debug|Release]
#   默认 Release

set -e
cd "$(dirname "$0")"

BUILD_TYPE="${1:-Release}"

# Release 默认开启 LTO，Debug 关闭
if [ "$BUILD_TYPE" = "Release" ]; then
    LTO=ON
else
    LTO=OFF
fi

echo "=============================================="
echo "  编译 IM Chat Server ($BUILD_TYPE, LTO=$LTO)"
echo "=============================================="
echo ""

# 检查工具链
for cmd in cmake ninja g++; do
    if ! command -v "$cmd" &>/dev/null; then
        echo "  [错误] 找不到 $cmd"
        echo ""
        echo "  请确保 MinGW64 工具链已安装并在 PATH 中。"
        echo "  MSYS2 用户请先执行:"
        echo "    export PATH=/c/msys64/mingw64/bin:\$PATH"
        echo "  （路径请替换为你的 MSYS2 安装位置）"
        exit 1
    fi
done

echo "  CMake:  $(cmake --version | head -1)"
echo "  Ninja:  $(ninja --version)"
echo "  g++:    $(g++ --dumpversion)"
echo ""

BUILD_DIR="build"

echo "  [1/3] CMake 配置..."
cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DENABLE_LTO="$LTO"

echo ""
echo "  [2/3] 编译..."
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE"

echo ""
echo "  [3/3] 拷贝到项目根目录..."
cp "$BUILD_DIR/im_chat_server.exe" ./im_chat_server.exe

echo ""
echo "=============================================="
echo "  编译成功! ($BUILD_TYPE)"
echo "  可执行文件: $BUILD_DIR/im_chat_server.exe"
echo "  启动: bash run.sh"
echo "=============================================="
