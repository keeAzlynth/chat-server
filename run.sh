#!/usr/bin/env bash
# IM Chat Server 启动脚本
# 用法: bash run.sh

set -e

# 切换到脚本所在目录
cd "$(dirname "$0")"

echo "=============================================="
echo "  IM Chat Server"
echo "  WebSocket: ws://127.0.0.1:8080"
echo "  Web 页面 : http://127.0.0.1:8081/"
echo "=============================================="
echo ""

# 查找 exe：优先 build 目录 > 项目根目录
EXE=""
for candidate in \
    "build/im_chat_server.exe" \
    "im_chat_server.exe"; do
    if [ -f "$candidate" ]; then
        EXE="$candidate"
        break
    fi
done

if [ -z "$EXE" ]; then
    echo "  [错误] 找不到 im_chat_server.exe"
    echo ""
    echo "  请先编译："
    echo "    bash build.sh"
    echo ""
    read -p "按回车键退出..."
    exit 1
fi

echo "  启动: $EXE"
echo "  配置: config/config.json"
echo ""

# 启动服务器
"$EXE" "config/config.json"
