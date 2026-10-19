# 贡献与反馈 / Contributing

## 中文

请先阅读 [README](README.md) 和[使用说明](docs/USER_GUIDE_2.3_zh-CN.md)。反馈时提供：

- 工具版本、安装包 SHA-256、Windows 版本和游戏构建号。
- 手柄型号、USB / 蓝牙连接，以及同时运行的其他 Mod。
- 复现步骤、预期和实际表现；关闭其他 Mod 后能否复现。

附日志前移除姓名、账户、订单、收款信息和本机私有路径。不要提交游戏本体、个人存档信息、支付凭据或私钥。

修改原创代码时遵循 [LICENSE.txt](LICENSE.txt)，保留第三方署名与许可，不引入未经授权的素材。说明改动原因和实际运行的检查；明确列出尚未完成的实机测试。构建入口为 `scripts/build-release.ps1`，源码归档入口为 `scripts/package-source.ps1`。

## English

Read the [README](README.en.md) and [guide](docs/USER_GUIDE_2.3_en-US.md) before reporting an issue. Include:

- Tool version, installer SHA-256, Windows version and game build.
- Controller model, USB/Bluetooth transport and other active mods.
- Reproduction steps, expected/actual behavior and whether it reproduces with other mods disabled.

Remove names, accounts, orders, payment details and private paths before sharing logs. Do not submit the game itself, personal save data, payment credentials or private keys.

Follow [LICENSE.txt](LICENSE.txt) for original-code changes. Preserve third-party licenses and attribution, and do not add unauthorized assets. Explain the change and checks actually run; state pending hardware tests. Build with `scripts/build-release.ps1`; package source with `scripts/package-source.ps1`.
