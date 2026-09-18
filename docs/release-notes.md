提供 Windows x86_64、Linux x86_64、macOS Apple Silicon 和 Intel 的独立运行包，以及 SHA-256 校验文件。

- Windows：解压后运行 `LogicAnalyzer.exe`。
- Linux：解压后运行 `LogicAnalyzer/LogicAnalyzer`；需要桌面环境，系统兼容基线为 Ubuntu 22.04（glibc 2.35）。
- macOS：解压后将 `LogicAnalyzer.app` 拖入应用程序目录。应用仅做本地临时签名，未进行 Apple Developer ID 签名或公证。

所有包均包含 Python 运行库与协议解码器，并在发布前运行启动自检。自检不验证真实 USB 设备或固件升级。

当前 CH32H417 通信使用 Windows 专属 CH375 驱动。Linux/macOS 支持演示信号、采集文件分析及协议解码；真实 CH32H417 设备通信暂未移植。Windows 使用设备时需安装厂家驱动及匹配的 CH375DLL（不随开源包分发）。
