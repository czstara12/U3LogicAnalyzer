# U3LogicAnalyzer

[![全平台构建与发布](https://github.com/czstara12/U3LogicAnalyzer/actions/workflows/build-release.yml/badge.svg)](https://github.com/czstara12/U3LogicAnalyzer/actions/workflows/build-release.yml)

基于 PulseView、libsigrok 和 libsigrokdecode 的 CH32H417 逻辑分析仪软件，包含演示驱动和协议解码器。

## 下载与平台支持

从 [Releases](https://github.com/czstara12/U3LogicAnalyzer/releases) 下载正式版或预发布版；每次主分支构建的测试包在 [Actions](https://github.com/czstara12/U3LogicAnalyzer/actions) 的 Artifacts 中。

| 平台 | 发布格式 | 功能范围 |
| --- | --- | --- |
| Windows x86_64 | ZIP 便携包 | CH32H417 采集、文件分析、演示和解码 |
| Linux x86_64 | tar.gz 便携包 | 文件分析、演示和解码 |
| macOS Apple Silicon / Intel | ZIP 内含 `.app` | 文件分析、演示和解码 |

CH32H417 通信使用 Windows 专属 CH375 API，Linux/macOS 尚未实现对应设备后端。Windows 连接设备需要另外安装厂家驱动，并提供匹配架构的 CH375DLL；仓库不包含该专有组件。IAP 固件升级及真实设备采集不在自动测试范围内。

Windows 解压后运行 `LogicAnalyzer.exe`。Linux 解压后运行 `LogicAnalyzer/LogicAnalyzer`，需要桌面环境及 glibc 2.35 或更高（以 Ubuntu 22.04 为构建基线）。macOS 将解压得到的 `LogicAnalyzer.app` 放入应用程序目录；应用目前使用临时签名，未进行 Developer ID 签名或公证。

发布包包含 Qt、Python 运行时、协议解码器和所需非系统动态库，无需预先安装 Python。每个压缩包附带 `.sha256` 校验文件。

## 本地构建

需要 CMake 3.18+、Ninja、C/C++ 编译器、Qt 5.12+、Boost、GLib/glibmm 2.4、libusb、HIDAPI、libzip 和 Python 3 开发库。

```bash
bash setup_env.sh
bash build.sh --package
```

- Windows：在 MSYS2 **MINGW64** 终端执行。安装脚本默认安装动态 Qt5；构建也兼容已有的 `qt5-static`。
- Linux：自动安装支持 Debian/Ubuntu，通过 `sudo apt-get` 安装依赖，默认使用 `/usr/bin/python3`。
- macOS：先安装 Xcode 命令行工具和 Homebrew，脚本使用 `qt@5`、`glibmm@2.66` 和 `python@3.13`。

常用参数：

```bash
bash build.sh                         # 增量编译并安装到 install-ci/
bash build.sh --clean                 # 清理编译产物后重新编译
bash build.sh --package --version 0.4.0-dev
BUILD_JOBS=4 bash build.sh --package   # 默认并行数为 2，避免编译大型表达式模板时耗尽内存
bash build.sh --build-dir build-local --prefix install-local --package
```

`build-ci/` 保存各组件构建结果，`install-ci/` 保存安装结果，`dist/` 保存最终压缩包。历史 `build_cmake/` 和 `install/` 不再作为默认输出；移动源码后也不会复用旧绝对路径缓存。直接运行软件建议使用解压后的便携包。

如只需重新打包，应使用编译解码库时的同一个 Python 和工具链：

```bash
python3 scripts/package.py --build-dir build-ci --prefix install-ci --version 0.4.0-dev
```

打包脚本在清除构建机 Python、Qt 和动态库环境变量后执行 `--smoke-test`，验证 Qt 初始化、libsigrok、Python 及非空协议解码器加载；任何失败都会阻止产物归档。`--skip-smoke-test` 仅供诊断，CI 不使用该参数。

Windows 还可安装 `mingw-w64-x86_64-nsis` 后执行 `bash package_installer.sh --version 0.4.0-dev` 生成安装器；已有同版本 ZIP 时添加 `--skip-build`。CI 默认分发 ZIP 便携包。

## 自动构建与发布

工作流位于 `.github/workflows/build-release.yml`：

1. 推送 `main`、提交 Pull Request 或手动运行 Actions：构建 Windows、Linux、macOS arm64 和 macOS x86_64，执行打包自检，并上传保留 14 天的 Artifacts。
2. 推送形如 `v0.4.0` 的标签：全部平台成功后，校验归档并自动创建 GitHub Release、上传压缩包及校验文件。
3. 形如 `v0.4.0-rc.1` 的标签自动标为预发布。某个平台失败时不会发布不完整版本。

```bash
git tag -a v0.4.0-rc.1 -m "发布 0.4.0 首个候选版本"
git push origin v0.4.0-rc.1
```

发布使用 GitHub 自动提供的 `GITHUB_TOKEN`，只有发布任务具有 `contents: write` 权限，无需配置个人访问令牌。标签应唯一；修改已发布版本请使用新标签。

工作流的运行器与工具链依据 [GitHub 官方运行器说明](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)、[MSYS2 CI 文档](https://www.msys2.org/docs/ci/) 和 [Homebrew Qt5 配方](https://formulae.brew.sh/formula/qt@5) 配置。Qt5 已进入 Homebrew 弃用周期，后续需要迁移 Qt6 或维护固定工具链。

## 源码与许可证

- `libsigrok/`：底层库、演示与 CH32H417 驱动。
- `libsigrok/bindings/cxx/`：C++ 绑定。
- `libsigrokdecode/`：Python 协议解码库与解码器。
- `pulseview/`：Qt 图形界面。
- `scripts/package.py`：跨平台依赖收集、启动检查及归档。

保留各上游项目的版权与许可证，详见各目录的 `COPYING`、`AUTHORS` 和源码文件头。发布包包含这些许可证副本。
