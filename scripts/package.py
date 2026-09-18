#!/usr/bin/env python3
"""@brief 收集运行时依赖，生成 Windows、Linux 和 macOS 便携发布包。

必须使用编译 libsigrokdecode 时对应的 Python 运行本脚本。缺失的动态库、
Qt 平台插件或 Python 标准库都会使打包失败，避免发布无法启动的压缩包。
"""

from __future__ import annotations

import argparse
from functools import lru_cache
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess
import sys
import sysconfig
import tarfile
import uuid
import zipfile


ROOT = Path(__file__).resolve().parents[1]
MACHO_MAGIC = {
    b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
    b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
    b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",
}
LINUX_SYSTEM = re.compile(
    r"^(?:linux-vdso|ld-linux|ld64|lib(?:c|m|dl|pthread|rt|resolv|util|nss_[^.]+))\."
)


def configure_output() -> None:
    """@brief 统一命令行输出编码，避免 Windows 本地代码页无法显示构建日志。"""
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")


def run(args: list[str | Path], *, env: dict[str, str] | None = None,
        timeout: int = 180) -> str:
    """@brief 执行命令，并在失败时显示完整诊断。"""
    command = [str(arg) for arg in args]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace", env=env, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"命令失败 ({result.returncode}): {command}\n{result.stdout}")
    return result.stdout


def tool(name: str) -> str:
    """@brief 查找必需工具，允许环境变量覆盖 Qt 工具路径。"""
    # MSYS2 为避免 Qt5/Qt6 冲突，使用 windeployqt-qt5 等带版本的命令。
    names = [f"{name}-qt5", name] if name in ("windeployqt", "macdeployqt", "qmake") else [name]
    candidate = os.environ.get(name.upper())
    if not candidate:
        candidate = next((found for variant in names if (found := shutil.which(variant))), None)
    if not candidate:
        for variant in names:
            sibling = Path(sys.executable).parent / (variant + ".exe" if os.name == "nt" else variant)
            if sibling.is_file():
                candidate = str(sibling)
                break
    if not candidate:
        raise RuntimeError(f"缺少打包工具：{name}")
    return candidate


def copy_file(source: Path, target: Path) -> None:
    """@brief 复制实际文件，避免软链接指向构建机。"""
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        if source.resolve() == target.resolve() or source.read_bytes() == target.read_bytes():
            return
        raise RuntimeError(f"运行库名称冲突：{source} -> {target}")
    shutil.copy2(source, target)


def python_runtime(home: Path) -> None:
    """@brief 按当前解释器布局复制完整标准库及其动态扩展。"""
    source = Path(sysconfig.get_path("stdlib"))
    if not (source / "encodings" / "__init__.py").is_file():
        raise RuntimeError(f"Python 标准库不完整：{source}")
    version = f"python{sys.version_info.major}.{sys.version_info.minor}"
    # MSYS2 与 Unix 都使用 lib/pythonX.Y；官方 Windows Python 使用 Lib。
    destination = home / ("Lib" if os.name == "nt" and source.name.lower() == "lib"
                          else f"lib/{version}")
    ignored = shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo", "site-packages",
                                     "dist-packages", "test", "tests", "idlelib",
                                     "tkinter", "turtledemo", "ensurepip", "config-*",
                                     "*.a")
    shutil.copytree(source, destination, ignore=ignored, dirs_exist_ok=True)
    # Debian 将动态扩展放在 platstdlib；Windows 官方版使用 DLLs。
    extension_dirs = [Path(sysconfig.get_config_var("DESTSHARED") or source / "lib-dynload")]
    if os.name == "nt":
        extension_dirs.append(Path(sys.base_prefix) / "DLLs")
    for extension_dir in extension_dirs:
        if extension_dir.is_dir() and extension_dir.resolve() != source.resolve():
            target = home / "DLLs" if extension_dir.name == "DLLs" else destination / "lib-dynload"
            shutil.copytree(extension_dir, target, ignore=ignored, dirs_exist_ok=True)
    if not any(path.suffix in (".so", ".pyd") for path in home.rglob("*")):
        raise RuntimeError("未找到 Python 动态扩展，请使用构建时的 Python 解释器")
    for source_license in [Path(sys.base_prefix) / "LICENSE.txt", source / "LICENSE.txt"]:
        if source_license.is_file():
            copy_file(source_license, home / "LICENSE.txt")
            break


def resources(prefix: Path, destination: Path) -> None:
    """@brief 安装解码器、项目许可证和便携包版本信息。"""
    decoders = prefix / "share/libsigrokdecode/decoders"
    if not (decoders / "uart" / "pd.py").is_file():
        raise RuntimeError(f"安装目录缺少协议解码器：{decoders}")
    shutil.copytree(decoders, destination / "share/libsigrokdecode/decoders",
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    for project in ("pulseview", "libsigrok", "libsigrokdecode"):
        for filename in ("COPYING", "COPYING.LESSER", "LICENSE"):
            license_file = ROOT / project / filename
            if license_file.is_file():
                copy_file(license_file, destination / "licenses" / f"{project}-{filename}")


def executable(prefix: Path, build_dir: Path, name: str) -> Path:
    """@brief 优先选择安装产物，兼容已有构建目录。"""
    candidates = [prefix / "bin" / name, build_dir / "logicanalyzer_build" / name]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise RuntimeError(f"找不到构建产物：{candidates}")


def binary_files(directory: Path, kind: str) -> list[Path]:
    """@brief 使用文件头识别原生二进制，避免把文本当作动态库处理。"""
    result = []
    for path in directory.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open("rb") as stream:
            magic = stream.read(4)
        if (kind == "pe" and magic[:2] == b"MZ" or
                kind == "elf" and magic == b"\x7fELF" or
                kind == "macho" and magic in MACHO_MAGIC):
            result.append(path)
    return result


def windows_dependencies(bundle: Path, prefix: Path) -> None:
    """@brief 递归分析 PE 导入表，收集 DLL 与 Python 扩展依赖。"""
    search_dirs = [prefix / "bin", Path(sys.executable).parent,
                   Path(sys.base_prefix) / "bin", Path(sys.base_prefix)]
    search_dirs.extend(Path(part) for part in os.environ.get("PATH", "").split(os.pathsep) if part)
    system_root = Path(os.environ.get("SystemRoot", "C:/Windows"))
    system_dirs = [system_root / "System32", system_root / "SysWOW64"]
    candidates: dict[str, Path] = {}
    for directory in search_dirs:
        if directory.is_dir():
            for path in directory.glob("*.dll"):
                candidates.setdefault(path.name.lower(), path)
    pending = binary_files(bundle, "pe")
    visited: set[Path] = set()
    objdump = tool("objdump")
    while pending:
        binary = pending.pop()
        if binary in visited:
            continue
        visited.add(binary)
        for name in re.findall(r"DLL Name:\s*(\S+)", run([objdump, "-p", binary])):
            target = bundle / name
            if target.exists():
                pending.append(target)
                continue
            # API 集与 Windows 系统 DLL 随操作系统提供，不能从构建机分发。
            if name.lower().startswith(("api-ms-win-", "ext-ms-win-")):
                continue
            if any((directory / name).is_file() for directory in system_dirs):
                continue
            source = candidates.get(name.lower())
            if source is None:
                raise RuntimeError(f"缺少 DLL：{name}（被 {binary} 引用）")
            copy_file(source, target)
            pending.append(target)


def package_windows(bundle: Path, prefix: Path, build_dir: Path) -> Path:
    """@brief 生成包含 Qt 插件、Python 与解码器的 Windows 目录。"""
    program = bundle / "LogicAnalyzer.exe"
    copy_file(executable(prefix, build_dir, program.name), program)
    python_runtime(bundle / "python")
    resources(prefix, bundle)
    imports = run([tool("objdump"), "-p", program])
    if re.search(r"DLL Name:\s*(?:lib)?Qt[56]", imports, re.IGNORECASE):
        # 本程序使用 QWidget 光栅绘制；MSYS2 不提供 Qt 官方包假定存在的 ANGLE。
        # 关闭可选图形后端的自动部署，真实 DLL 导入仍由后续递归扫描严格检查。
        run([tool("windeployqt"), "--release", "--no-translations", "--compiler-runtime",
             "--no-angle", "--no-opengl-sw",
             "--dir", bundle, program])
        if not (bundle / "platforms/qwindows.dll").is_file():
            raise RuntimeError("windeployqt 未部署 Windows 平台插件")
    windows_dependencies(bundle, prefix)
    (bundle / "qt.conf").write_text("[Paths]\nPrefix=.\nPlugins=.\n", encoding="utf-8")
    (bundle / "run.bat").write_text('@echo off\nset "PATH=%~dp0;%SystemRoot%\\System32;%SystemRoot%"\n'
                                     '"%~dp0LogicAnalyzer.exe" %*\n', encoding="utf-8")
    return program


def linux_dependencies(bundle: Path, prefix: Path) -> None:
    """@brief 收集 ELF 依赖，并给每个二进制设置相对运行库搜索路径。"""
    library_dir = bundle / "lib"
    library_dir.mkdir(exist_ok=True)
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = os.pathsep.join([str(library_dir), str(prefix / "lib"),
                                              str(prefix / "lib64"), env.get("LD_LIBRARY_PATH", "")])
    pending = binary_files(bundle, "elf")
    visited: set[Path] = set()
    while pending:
        binary = pending.pop()
        if binary in visited:
            continue
        visited.add(binary)
        output = run([tool("ldd"), binary], env=env)
        if "not found" in output:
            raise RuntimeError(f"存在缺失的 ELF 依赖：{binary}\n{output}")
        for line in output.splitlines():
            match = re.match(r"\s*(\S+) => (/.+?) \(", line)
            if not match:
                continue
            name, source_name = match.groups()
            if LINUX_SYSTEM.match(name):
                continue
            target = library_dir / name
            copy_file(Path(source_name), target)
            pending.append(target)
    for binary in visited:
        # 软件包管理器可能安装只读库，仅修改包内副本的权限。
        binary.chmod(binary.stat().st_mode | stat.S_IWUSR)
        relative = os.path.relpath(library_dir, binary.parent)
        run([tool("patchelf"), "--set-rpath", f"$ORIGIN/{relative}", binary])


def package_linux(bundle: Path, prefix: Path, build_dir: Path) -> Path:
    """@brief 生成 Linux 便携目录，使用启动器定位共享库。"""
    program = bundle / "bin/LogicAnalyzer"
    copy_file(executable(prefix, build_dir, "LogicAnalyzer"), program)
    python_runtime(bundle)
    resources(prefix, bundle)
    plugin_dir = Path(run([tool("qmake"), "-query", "QT_INSTALL_PLUGINS"]).strip())
    for category in ("platforms", "imageformats", "iconengines", "xcbglintegrations"):
        source = plugin_dir / category
        if source.is_dir():
            shutil.copytree(source, bundle / "plugins" / category)
    for required in ("libqxcb.so", "libqoffscreen.so"):
        if not (bundle / "plugins/platforms" / required).is_file():
            raise RuntimeError(f"缺少 Qt 平台插件：{required}")
    linux_dependencies(bundle, prefix)
    (bundle / "bin/qt.conf").write_text("[Paths]\nPrefix=..\nPlugins=plugins\n", encoding="utf-8")
    launcher = bundle / "LogicAnalyzer"
    launcher.write_text('#!/bin/sh\nset -eu\n'
                        'ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
                        'export LD_LIBRARY_PATH="$ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n'
                        'export QT_PLUGIN_PATH="$ROOT/plugins"\n'
                        'exec "$ROOT/bin/LogicAnalyzer" "$@"\n', encoding="utf-8")
    launcher.chmod(0o755)
    return launcher


@lru_cache(maxsize=None)
def macho_rpaths(binary: Path) -> list[str]:
    """@brief 读取 Mach-O 的 LC_RPATH，供运行库解析使用。"""
    output = run([tool("otool"), "-l", binary])
    return re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.+?) \(offset", output)


@lru_cache(maxsize=None)
def macho_uuids(binary: Path) -> tuple[str, ...]:
    """@brief 缓存 Mach-O UUID；修改加载路径和签名不会改变链接器 UUID。"""
    return tuple(sorted(re.findall(r"\buuid ([A-Fa-f0-9-]+)",
                                   run([tool("otool"), "-l", binary]))))


def same_macho_image(source: Path, deployed: Path) -> bool:
    """@brief 用链接器 UUID 识别已被部署工具重定位的同一动态库。"""
    source_ids = macho_uuids(source)
    return bool(source_ids) and source_ids == macho_uuids(deployed)


def macos_library_paths(prefix: Path) -> list[Path]:
    """@brief 收集构建前缀、Qt、Python 和 Homebrew 各组件的原始库目录。"""
    candidates = [prefix / "lib", Path(sysconfig.get_config_var("LIBDIR") or sys.base_prefix),
                  Path(sys.base_prefix), Path(sysconfig.get_config_var("DESTSHARED") or sys.base_prefix),
                  Path(run([tool("qmake"), "-query", "QT_INSTALL_LIBS"]).strip())]
    if shutil.which("brew"):
        brew_prefix = Path(run([tool("brew"), "--prefix"]).strip())
        candidates.extend([brew_prefix / "lib", brew_prefix / "Frameworks"])
        # keg-only 组件不一定链接到 Homebrew/lib，例如独立版本的 Python 和 OpenSSL。
        candidates.extend(sorted((brew_prefix / "opt").glob("*/lib")))
    return list(dict.fromkeys(path.resolve() for path in candidates if path.is_dir()))


def macos_dependencies(app: Path, prefix: Path, original_app: Path) -> None:
    """@brief 递归迁移非系统 dylib，并改成相对加载路径。"""
    frameworks = app / "Contents/Frameworks"
    frameworks.mkdir(exist_ok=True)
    app_executable = app / "Contents/MacOS/LogicAnalyzer"
    exe_rpaths = macho_rpaths(app_executable)
    search_dirs = macos_library_paths(prefix)
    sources: dict[Path, Path] = {}
    origins: dict[Path, Path] = {}
    binaries = [path.resolve() for path in binary_files(app, "macho")]
    # macdeployqt 已复制并修改部分库；先恢复来源，才能解释其 @loader_path/LC_RPATH。
    for binary in binaries:
        relative = binary.relative_to(app.resolve())
        candidates = [original_app / relative]
        if binary.is_relative_to(frameworks.resolve()):
            framework_relative = binary.relative_to(frameworks.resolve())
            candidates.extend(directory / framework_relative for directory in search_dirs)
        candidates.extend(directory / binary.name for directory in search_dirs)
        original = next((path.resolve() for path in candidates if path.is_file()
                         and same_macho_image(path.resolve(), binary)), binary)
        origins[binary] = original
        sources[original] = binary
    pending = [(path, origins[path]) for path in binaries]
    visited: set[Path] = set()

    def expand(value: str, owner: Path) -> Path:
        return Path(value.replace("@loader_path", str(owner.parent))
                    .replace("@executable_path", str(app_executable.parent)))

    while pending:
        binary, original = pending.pop()
        if binary in visited:
            continue
        visited.add(binary)
        # Homebrew 的库通常只读，重定位前允许写入包内 Mach-O 副本。
        binary.chmod(binary.stat().st_mode | stat.S_IWUSR)
        dependencies = [line for line in run([tool("otool"), "-L", binary]).splitlines()
                        if " (compatibility version " in line]
        identities = run([tool("otool"), "-D", binary]).splitlines()[1:]
        rpaths = macho_rpaths(original) + exe_rpaths
        for line in dependencies:
            dependency = line.strip().split(" (compatibility version", 1)[0]
            if dependency in identities:
                continue
            if dependency.startswith(("/System/Library/", "/usr/lib/")):
                continue
            candidates = [expand(dependency, original), expand(dependency, binary)]
            if dependency.startswith("@rpath/"):
                tail = dependency[len("@rpath/"):]
                candidates = [expand(path, owner) / tail for owner in (original, binary)
                              for path in rpaths]
                candidates += [frameworks / tail, original.parent / tail]
                candidates.extend(directory / tail for directory in search_dirs)
            if dependency.startswith("/"):
                candidates.insert(0, Path(dependency))
            source = next((path.resolve() for path in candidates if path.is_file()), None)
            # dylib 的第一项可能是自己的 install name，不属于外部依赖。
            if source == original.resolve() or source == binary.resolve():
                continue
            if source is None:
                raise RuntimeError(f"缺少 Mach-O 依赖：{dependency}（被 {binary} 引用）")
            if source.is_relative_to(app.resolve()):
                target = source
                source = origins.get(target, source)
            else:
                target = sources.get(source)
                if target is None:
                    target = frameworks / source.name
                    # macdeployqt 可能已修改同一库的加载路径，不能再用字节相等判断。
                    if not (target.is_file() and same_macho_image(source, target)):
                        copy_file(source, target)
                    sources[source] = target
                    origins[target] = source
            replacement = "@loader_path/" + os.path.relpath(target, binary.parent)
            run([tool("install_name_tool"), "-change", dependency, replacement, binary])
            pending.append((target, source))
        # 主程序没有 LC_ID_DYLIB；仅为真正的动态库更新标识。
        if identities:
            run([tool("install_name_tool"), "-id", f"@rpath/{binary.name}", binary])
    # 签名 app 主程序会校验整个包，必须先完成所有嵌套库和插件的签名。
    # 主程序与 Frameworks 内 dylib 的目录深度相同，不能仅按深度决定先后。
    nested_binaries = visited - {app_executable.resolve()}
    for binary in sorted(nested_binaries, key=lambda path: (-len(path.parts), str(path))):
        run([tool("codesign"), "--force", "--sign", "-", binary])
    for framework in sorted(frameworks.glob("*.framework")):
        run([tool("codesign"), "--force", "--sign", "-", framework])
    run([tool("codesign"), "--force", "--sign", "-", app])
    run([tool("codesign"), "--verify", "--deep", "--strict", app])


def package_macos(bundle: Path, prefix: Path, build_dir: Path) -> Path:
    """@brief 部署 macOS 应用包、Python 运行库和本地签名。"""
    candidates = [prefix / "LogicAnalyzer.app",
                  build_dir / "logicanalyzer_build/LogicAnalyzer.app"]
    source = next((path for path in candidates if path.is_dir()), None)
    if source is None:
        raise RuntimeError(f"找不到 macOS 应用包：{candidates}")
    app = bundle / "LogicAnalyzer.app"
    shutil.copytree(source, app, symlinks=True)
    resources_dir = app / "Contents/Resources"
    resources_dir.mkdir(exist_ok=True)
    python_runtime(resources_dir)
    resources(prefix, resources_dir)
    run([tool("macdeployqt"), app, "-always-overwrite", "-no-strip",
         f"-libpath={prefix / 'lib'}"], timeout=600)
    macos_dependencies(app, prefix, source)
    return app / "Contents/MacOS/LogicAnalyzer"


def smoke_test(program: Path, bundle: Path) -> None:
    """@brief 清除构建环境后检查 GUI、Python 和解码器能否独立加载。"""
    env = os.environ.copy()
    for key in ("PYTHONHOME", "PYTHONPATH", "SIGROKDECODE_DIR", "QT_PLUGIN_PATH",
                "QT_QPA_PLATFORM_PLUGIN_PATH", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH",
                "DYLD_FRAMEWORK_PATH"):
        env.pop(key, None)
    # Linux 构建机通常没有显示服务；Windows/macOS 的无窗口检查使用原生插件。
    env["QT_QPA_PLATFORM"] = {"Windows": "windows", "Darwin": "cocoa",
                              "Linux": "offscreen"}[platform.system()]
    if platform.system() == "Darwin":
        # 由应用自身禁用缓存写入，避免测试环境掩盖已签名资源被修改的问题。
        env.pop("PYTHONDONTWRITEBYTECODE", None)
    else:
        env["PYTHONDONTWRITEBYTECODE"] = "1"
    if os.name == "nt":
        system_root = env.get("SystemRoot", "C:/Windows")
        env["PATH"] = os.pathsep.join([str(bundle), str(Path(system_root) / "System32"), system_root])
    else:
        env["PATH"] = "/usr/bin:/bin"
    print(run([program, "--smoke-test"], env=env, timeout=90), flush=True)


def archive(bundle: Path, output: Path, name: str, system: str) -> Path:
    """@brief 创建发布归档并附加 SHA-256 校验文件。"""
    if system == "Linux":
        target = output / f"{name}.tar.gz"
        with tarfile.open(target, "w:gz") as stream:
            stream.add(bundle, arcname="LogicAnalyzer")
    elif system == "Darwin":
        target = output / f"{name}.zip"
        run([tool("ditto"), "-c", "-k", "--sequesterRsrc", "--keepParent",
             bundle / "LogicAnalyzer.app", target], timeout=600)
    else:
        target = output / f"{name}.zip"
        with zipfile.ZipFile(target, "w", compression=zipfile.ZIP_DEFLATED) as stream:
            for path in sorted(bundle.rglob("*")):
                if path.is_file():
                    stream.write(path, Path("LogicAnalyzer") / path.relative_to(bundle))
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    target.with_name(target.name + ".sha256").write_bytes(f"{digest}  {target.name}\n".encode("utf-8"))
    return target


def main() -> None:
    """@brief 解析构建路径，打包当前平台，并默认执行独立启动检查。"""
    configure_output()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path,
                        default=Path(os.environ.get("BUILD_DIR", ROOT / "build-ci")))
    parser.add_argument("--prefix", type=Path,
                        default=Path(os.environ.get("PREFIX", ROOT / "install-ci")))
    parser.add_argument("--output-dir", type=Path, default=ROOT / "dist")
    parser.add_argument("--version", default=os.environ.get("VERSION", "dev"))
    parser.add_argument("--skip-smoke-test", action="store_true",
                        help="仅用于诊断；正式发布不得跳过运行验证")
    args = parser.parse_args()
    system = platform.system()
    packagers = {"Windows": package_windows, "Linux": package_linux, "Darwin": package_macos}
    if system not in packagers:
        raise RuntimeError(f"不支持的平台：{system}")
    version = re.sub(r"[^A-Za-z0-9._-]", "-", args.version)
    architecture = {"AMD64": "x86_64", "aarch64": "arm64"}.get(platform.machine(), platform.machine())
    system_name = {"Windows": "windows", "Linux": "linux", "Darwin": "macos"}[system]
    name = f"LogicAnalyzer-{version}-{system_name}-{architecture}"
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # 临时目录只清理本次生成内容，保护用户已有的 dist 文件。
    temporary = output / f".package-{uuid.uuid4().hex}"
    # 继承输出目录权限，兼容 MSYS2 Python 在 Windows 沙箱中的 ACL。
    temporary.mkdir()
    try:
        bundle = temporary / "LogicAnalyzer"
        bundle.mkdir()
        program = packagers[system](bundle, args.prefix.resolve(), args.build_dir.resolve())
        metadata = {"version": args.version, "platform": system_name, "architecture": architecture,
                    "python": platform.python_version()}
        metadata_dir = bundle / "LogicAnalyzer.app/Contents/Resources" if system == "Darwin" else bundle
        (metadata_dir / "build-info.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n",
                                                     encoding="utf-8")
        # macOS 签名覆盖 Resources 内容，写完版本信息后重新签名最外层应用。
        if system == "Darwin":
            run([tool("codesign"), "--force", "--sign", "-", bundle / "LogicAnalyzer.app"])
        if not args.skip_smoke_test:
            smoke_test(program, bundle)
            if system == "Darwin":
                app = bundle / "LogicAnalyzer.app"
                caches = [path for path in app.rglob("*")
                          if path.name == "__pycache__" or path.suffix in (".pyc", ".pyo")]
                if caches:
                    raise RuntimeError(f"启动后签名包中出现 Python 缓存：{caches[:10]}")
                run([tool("codesign"), "--verify", "--deep", "--strict", app])
                print("启动后签名验证通过，未生成 Python 缓存。", flush=True)
        target = archive(bundle, output, name, system)
        print(f"已生成发布包：{target}", flush=True)
    finally:
        if temporary.resolve().parent != output:
            raise RuntimeError(f"拒绝清理输出目录以外的路径：{temporary}")
        shutil.rmtree(temporary)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        print(f"打包失败：{error}", file=sys.stderr)
        sys.exit(1)
