#!/usr/bin/env bash
# 安装原生构建和打包依赖，不修改终端启动配置。
set -euo pipefail
case "$(uname -s)" in
    MINGW*|MSYS*)
        [[ "${MSYSTEM:-}" == MINGW64 ]] || { echo "请使用 MSYS2 MINGW64 终端。" >&2; exit 1; }
        pacman -S --needed --noconfirm \
            mingw-w64-x86_64-{gcc,cmake,ninja,pkgconf} \
            mingw-w64-x86_64-{glib2,glibmm,libusb,hidapi,libzip,boost,python} \
            mingw-w64-x86_64-{qt5-base,qt5-svg,qt5-tools}
        ;;
    Linux)
        command -v apt-get >/dev/null || { echo "自动安装支持 Debian/Ubuntu；其他发行版请按 README 安装依赖。" >&2; exit 1; }
        sudo apt-get update
        sudo apt-get install -y build-essential cmake ninja-build pkg-config \
            libglib2.0-dev libglibmm-2.4-dev libusb-1.0-0-dev libhidapi-dev libzip-dev \
            libboost-filesystem-dev libboost-serialization-dev \
            qtbase5-dev libqt5svg5-dev qttools5-dev qttools5-dev-tools \
            python3-dev patchelf
        ;;
    Darwin)
        command -v brew >/dev/null || { echo "请先安装 Homebrew。" >&2; exit 1; }
        brew install cmake ninja pkgconf glib glibmm@2.66 libusb hidapi libzip boost qt@5
        # 构建使用专用前缀，避免覆盖 runner 或用户已有的 Python 命令。
        brew install --skip-link python@3.13
        ;;
    *) echo "不支持的系统：$(uname -s)" >&2; exit 1 ;;
esac
echo "依赖准备完成。执行 bash build.sh --package 开始构建和打包。"
