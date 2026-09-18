# CH32H417 LogicAnalyzer 构建说明

构建与发布入口已统一为 Windows、Linux、macOS 三平台。请参阅 [完整说明](README.md)。

```bash
bash setup_env.sh
bash build.sh --package
```

默认产物位于 `dist/`；Linux/macOS 当前支持演示、文件分析和解码，CH32H417 实机采集仍使用 Windows 专属驱动。
