#!/usr/bin/env bash
# 统一构建入口：Windows 使用 MSYS2 MINGW64，Linux/macOS 使用 Bash。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/build-ci}"
PREFIX="${PREFIX:-${SCRIPT_DIR}/install-ci}"
OUTPUT_DIR="${OUTPUT_DIR:-${SCRIPT_DIR}/dist}"
VERSION="${VERSION:-0.4.0-dev}"
CLEAN=0
PACKAGE=0
while (( $# )); do
    case "$1" in
        --clean) CLEAN=1; shift ;;
        --package) PACKAGE=1; shift ;;
        --prefix|--build-dir|--output-dir|--version)
            [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || { echo "参数 $1 缺少值" >&2; exit 2; }
            case "$1" in
                --prefix) PREFIX="$2" ;;
                --build-dir) BUILD_DIR="$2" ;;
                --output-dir) OUTPUT_DIR="$2" ;;
                --version) VERSION="$2" ;;
            esac
            shift 2 ;;
        --help|-h)
            echo "用法：bash build.sh [--clean] [--package] [--prefix 路径] [--build-dir 路径] [--output-dir 路径] [--version 版本]"
            exit 0 ;;
        *) echo "未知参数：$1" >&2; exit 2 ;;
    esac
done
case "$(uname -s)" in
    MINGW*|MSYS*)
        [[ "${MSYSTEM:-}" == MINGW64 ]] || { echo "请使用 MSYS2 MINGW64 终端。" >&2; exit 1; }
        export PATH="/mingw64/bin:/usr/bin:$PATH"
        if [[ ! -f /mingw64/lib/cmake/Qt5/Qt5Config.cmake && -d /mingw64/qt5-static ]]; then
            export PATH="/mingw64/qt5-static/bin:$PATH"
            export CMAKE_PREFIX_PATH="/mingw64/qt5-static${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
        fi
        ;;
    Darwin)
        if command -v brew >/dev/null; then
            QT_PREFIX="$(brew --prefix qt@5)"
            PY_PREFIX="$(brew --prefix python@3.13)"
            export PATH="$QT_PREFIX/bin:$PY_PREFIX/libexec/bin:$PATH"
            export CMAKE_PREFIX_PATH="$QT_PREFIX${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
            export PKG_CONFIG_PATH="$(brew --prefix glibmm@2.66)/lib/pkgconfig:$(brew --prefix libsigc++@2)/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
        fi
        ;;
    Linux) PYTHON="${PYTHON:-/usr/bin/python3}" ;;
    *) echo "不支持的系统：$(uname -s)" >&2; exit 1 ;;
esac
for tool in cmake ninja pkg-config; do
    command -v "$tool" >/dev/null || { echo "缺少 $tool，请先执行 bash setup_env.sh。" >&2; exit 1; }
done
PYTHON="${PYTHON:-$(command -v python3 || command -v python)}"
JOBS="${BUILD_JOBS:-2}"
mkdir -p "$BUILD_DIR" "$PREFIX" "$OUTPUT_DIR"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
PREFIX="$(cd "$PREFIX" && pwd)"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export CMAKE_PREFIX_PATH="$PREFIX${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
COMMON=(-G Ninja "-DCMAKE_INSTALL_PREFIX=$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_BUILD_TYPE=Release -DDISABLE_WERROR=ON)
BUILD_FLAGS=()
if (( CLEAN )); then
    # 让构建工具重新编译，不递归删除用户目录，兼容 CMake 3.18。
    BUILD_FLAGS=(--clean-first)
fi
build_component() {
    local source="$1" name="$2"
    shift 2
    echo "===== 编译 $name ====="
    cmake -S "$SCRIPT_DIR/$source" -B "$BUILD_DIR/$name" "${COMMON[@]}" "$@"
    cmake --build "$BUILD_DIR/$name" "${BUILD_FLAGS[@]}" --parallel "$JOBS"
    cmake --install "$BUILD_DIR/$name"
}
build_component libsigrok libsigrok_build
build_component libsigrok/bindings/cxx libsigrokcxx_build
build_component libsigrokdecode libsigrokdecode_build "-DPython3_EXECUTABLE=$PYTHON"
build_component pulseview logicanalyzer_build \
    -DSTATIC_PKGDEPS_LIBS=OFF -DENABLE_DECODE=ON -DENABLE_FLOW=OFF \
    -DENABLE_TESTS=OFF -DENABLE_SIGNALS=OFF
echo "构建完成：$PREFIX"
if (( PACKAGE )); then
    "$PYTHON" "$SCRIPT_DIR/scripts/package.py" --build-dir "$BUILD_DIR" \
        --prefix "$PREFIX" --output-dir "$OUTPUT_DIR" --version "$VERSION"
fi
