#!/usr/bin/env bash
# 从同版本 Windows 便携 ZIP 生成 NSIS 安装包，需在 MSYS2 MINGW64 中运行。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/build-ci}"
PREFIX="${PREFIX:-${SCRIPT_DIR}/install-ci}"
OUTPUT_DIR="${OUTPUT_DIR:-${SCRIPT_DIR}/dist}"
VERSION="${VERSION:-0.4.0-dev}"
SKIP_BUILD=0
while (( $# )); do
    case "$1" in
        --skip-build) SKIP_BUILD=1; shift ;;
        --version|--build-dir|--prefix|--output-dir)
            [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || { echo "参数 $1 缺少值" >&2; exit 2; }
            case "$1" in
                --version) VERSION="$2" ;;
                --build-dir) BUILD_DIR="$2" ;;
                --prefix) PREFIX="$2" ;;
                --output-dir) OUTPUT_DIR="$2" ;;
            esac
            shift 2 ;;
        --help|-h)
            echo "用法：bash package_installer.sh [--skip-build] [--version 版本] [--build-dir 路径] [--prefix 路径] [--output-dir 路径]"
            echo "默认版本：0.4.0-dev；--skip-build 使用输出目录中相同版本的 Windows ZIP。"
            exit 0 ;;
        *) echo "未知参数：$1" >&2; exit 2 ;;
    esac
done
[[ "${MSYSTEM:-}" == MINGW64 ]] || { echo "请使用 MSYS2 MINGW64 终端。" >&2; exit 1; }
export PATH="/mingw64/bin:/usr/bin:$PATH"
command -v makensis >/dev/null || {
    echo "缺少 NSIS；请先安装：pacman -S --needed mingw-w64-x86_64-nsis" >&2
    exit 1
}
PYTHON="${PYTHON:-$(command -v python3 || command -v python)}"
if (( ! SKIP_BUILD )); then
    bash "$SCRIPT_DIR/build.sh" --package --build-dir "$BUILD_DIR" --prefix "$PREFIX" \
        --output-dir "$OUTPUT_DIR" --version "$VERSION"
fi
"$PYTHON" "$SCRIPT_DIR/scripts/installer.py" --build-dir "$BUILD_DIR" \
    --output-dir "$OUTPUT_DIR" --version "$VERSION"
