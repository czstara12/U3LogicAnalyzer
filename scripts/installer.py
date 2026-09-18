#!/usr/bin/env python3
"""@brief 从经过验证的 Windows 便携包生成同版本 NSIS 安装程序。"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import zipfile

from package import ROOT, configure_output, run, tool


def nsis_quote(value: str | Path) -> str:
    """@brief 转义 NSIS 字面值，避免工作目录中的美元符号被解释为变量。"""
    return str(value).replace("$", "$$").replace('"', '$\\"')


def prepare_installer(build_dir: Path, output_dir: Path, version: str) -> tuple[Path, Path]:
    """@brief 校验同版本 ZIP，安全更新专用目录，并生成安装与逐文件卸载脚本。"""
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", version):
        raise RuntimeError("版本只能包含英文字母、数字、点、下划线和连字符")
    build_dir = build_dir.resolve()
    output_dir = output_dir.resolve()
    if not build_dir.is_relative_to(ROOT) or build_dir == ROOT:
        raise RuntimeError(f"安装器暂存目录必须位于项目的构建子目录：{build_dir}")
    stage = build_dir / "installer-stage"
    if stage.is_symlink() or stage.resolve().parent != build_dir:
        raise RuntimeError(f"拒绝使用重定向的安装器暂存目录：{stage}")
    if output_dir.is_relative_to(stage):
        raise RuntimeError(f"发布输出目录不能位于安装器暂存目录中：{output_dir}")
    source = output_dir / f"LogicAnalyzer-{version}-windows-x86_64.zip"
    if not source.is_file():
        raise RuntimeError(f"缺少同版本 Windows 便携包：{source}；请先运行 build.sh --package --version {version}")
    checksum = source.with_name(source.name + ".sha256")
    if checksum.is_file():
        expected = checksum.read_text(encoding="utf-8").split()
        if not expected or expected[0] != hashlib.sha256(source.read_bytes()).hexdigest():
            raise RuntimeError(f"ZIP 校验值不匹配：{source}")
    with zipfile.ZipFile(source) as archive:
        entries = archive.infolist()
        for entry in entries:
            path = PurePosixPath(entry.filename)
            if (path.is_absolute() or not path.parts or path.parts[0] != "LogicAnalyzer"
                    or ".." in path.parts or "\\" in entry.filename or ":" in entry.filename
                    or any(part.endswith((" ", ".")) for part in path.parts)
                    or stat.S_ISLNK(entry.external_attr >> 16)):
                raise RuntimeError(f"ZIP 包含不安全路径：{entry.filename}")
        metadata = json.loads(archive.read("LogicAnalyzer/build-info.json"))
        if metadata.get("version") != version or metadata.get("platform") != "windows" \
                or metadata.get("architecture") != "x86_64":
            raise RuntimeError(f"ZIP 构建信息与指定版本或平台不一致：{metadata}")
        if "LogicAnalyzer/LogicAnalyzer.exe" not in archive.namelist():
            raise RuntimeError("ZIP 缺少 LogicAnalyzer.exe")
        marker = stage / ".logicanalyzer-installer-stage"
        if stage.exists():
            if not marker.is_file():
                raise RuntimeError(f"暂存目录包含非本脚本管理的内容，拒绝清理：{stage}")
            # 路径已确认位于当前项目构建目录，且带有本脚本专用标记。
            shutil.rmtree(stage)
        stage.mkdir(parents=True)
        marker.write_text("LogicAnalyzer 安装器专用暂存目录\n", encoding="utf-8")
        archive.extractall(stage)
    payload = stage / "LogicAnalyzer"
    target = output_dir / f"LogicAnalyzer-{version}-windows-x86_64-setup.exe"
    script = stage / "setup.nsi"
    # 卸载仅移除本安装包的文件，保留用户后来添加的数据。
    uninstall = ['    Delete "$INSTDIR\\uninstall.exe"']
    for path in sorted(payload.rglob("*"), key=lambda item: len(item.parts), reverse=True):
        relative = nsis_quote(str(path.relative_to(payload)).replace("/", "\\"))
        command = "RMDir" if path.is_dir() else "Delete"
        uninstall.append(f'    {command} "$INSTDIR\\{relative}"')
    uninstall.append('    RMDir "$INSTDIR"')
    script.write_text(f'''Unicode true
!include "MUI2.nsh"
Name "LogicAnalyzer"
OutFile "{nsis_quote(target)}"
InstallDir "$PROGRAMFILES64\\LogicAnalyzer"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"

Function .onInit
    SetRegView 64
    ReadRegStr $0 HKLM "Software\\LogicAnalyzer" "InstallDir"
    StrCmp $0 "" +2
    StrCpy $INSTDIR $0
FunctionEnd

Section "Install"
    SetRegView 64
    SetShellVarContext all
    SetOutPath "$INSTDIR"
    File /r "{nsis_quote(payload)}\\*"
    CreateDirectory "$SMPROGRAMS\\LogicAnalyzer"
    CreateShortCut "$SMPROGRAMS\\LogicAnalyzer\\LogicAnalyzer.lnk" "$INSTDIR\\LogicAnalyzer.exe"
    CreateShortCut "$SMPROGRAMS\\LogicAnalyzer\\Uninstall.lnk" "$INSTDIR\\uninstall.exe"
    CreateShortCut "$DESKTOP\\LogicAnalyzer.lnk" "$INSTDIR\\LogicAnalyzer.exe"
    WriteUninstaller "$INSTDIR\\uninstall.exe"
    WriteRegStr HKLM "Software\\LogicAnalyzer" "InstallDir" "$INSTDIR"
    WriteRegStr HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LogicAnalyzer" "DisplayName" "LogicAnalyzer"
    WriteRegStr HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LogicAnalyzer" "UninstallString" '"$INSTDIR\\uninstall.exe"'
    WriteRegStr HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LogicAnalyzer" "DisplayVersion" "{nsis_quote(version)}"
    WriteRegStr HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LogicAnalyzer" "DisplayIcon" "$INSTDIR\\LogicAnalyzer.exe"
SectionEnd

Section "Uninstall"
    SetRegView 64
    SetShellVarContext all
    SetOutPath "$TEMP"
{chr(10).join(uninstall)}
    Delete "$SMPROGRAMS\\LogicAnalyzer\\LogicAnalyzer.lnk"
    Delete "$SMPROGRAMS\\LogicAnalyzer\\Uninstall.lnk"
    RMDir "$SMPROGRAMS\\LogicAnalyzer"
    Delete "$DESKTOP\\LogicAnalyzer.lnk"
    DeleteRegKey HKLM "Software\\LogicAnalyzer"
    DeleteRegKey HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LogicAnalyzer"
SectionEnd
''', encoding="utf-8-sig")
    return script, target


def main() -> None:
    """@brief 生成 NSIS 安装包，并写入 SHA-256 校验文件。"""
    configure_output()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-ci")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "dist")
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    compiler = tool("makensis")
    script, target = prepare_installer(args.build_dir, args.output_dir, args.version)
    print(run([compiler, "/V2", script], timeout=600), flush=True)
    if not target.is_file():
        raise RuntimeError(f"NSIS 未生成预期安装包：{target}")
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    target.with_name(target.name + ".sha256").write_bytes(f"{digest}  {target.name}\n".encode("utf-8"))
    print(f"已生成安装包：{target}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, KeyError, zipfile.BadZipFile,
            subprocess.TimeoutExpired) as error:
        print(f"安装包生成失败：{error}", file=sys.stderr)
        sys.exit(1)
