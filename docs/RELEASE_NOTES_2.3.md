# 2.3 · 免费双语版 / Free bilingual edition

整理日期 / Prepared: **2026-10-16 (Asia/Hong_Kong)**  
产品版本 / Product: **2.3** · 内核 / Bridge: **4.1**

## 中文

面向 PC 版《鬼武者 剑之道》的非官方 DualSense 增强工具，提供游戏事件触觉、自适应扳机、手柄音频和 Windows 图形管理器。

### 本版内容

- 中文 / English 合并为一个安装包，安装时可选语言，管理器内可切换保存。
- 移除激活码、设备绑定及十分钟试用限制，无需授权服务器或发码工具。
- 提供 DualSense USB / 蓝牙触觉、扳机与手柄声音；保留 Edge USB，Edge 蓝牙待实机验证。
- 加入支付宝 / 微信自愿捐赠与禁止倒卖、冒充公告；捐赠不解锁功能。
- 修复同一 Steam 路径因大小写、斜杠写法等被重复识别而导致自动定位失败的问题；多个真实副本仍交由用户选择。
- 移除捐赠页两个“查看完整收款码”按钮及大图弹窗，直接保留页面二维码。

这些维护更新沿用 2.3 文件名，请以本次 SHA-256 清单确认具体构建。安装包保留此前完成的最终二进制；本次源码包另外补充中英文介绍、截图和发行文档，未改动运行代码。

### 文件与安装

| 文件 | 说明 |
| --- | --- |
| `Onimusha-PS5-Adapter-Setup-2.3.exe` | Windows x64 双语安装包，包含运行时和现有 payload |
| `Onimusha-PS5-Adapter-Source-2.3.zip` | 可审阅源码、构建脚本、依赖和素材 |
| `SHA256SUMS-2.3.txt` | 两个文件的 SHA-256 |

关闭游戏后运行安装包。自动定位失败时，选择包含 `OnimushaWotS.exe` 的文件夹，点“安装 / 修复”。Steam 游戏属性 → 控制器中保持“启用 Steam 输入”。[完整说明](USER_GUIDE_2.3_zh-CN.md)

### 验证与限制

已有构建、离线回归和界面检查见 [RELEASE_QA_2.3.md](RELEASE_QA_2.3.md)。GitHub 整理另行核对源码清单、包哈希、上传附件及草稿状态；没有重跑完整游戏或所有硬件测试。

尚未完成干净机器安装矩阵、Edge 蓝牙和多设备长期稳定性验证。USB 与蓝牙不保证相同音质、延迟或体验。本版未进行代码签名。

### 许可与支持

原创代码采用“源码公开、禁止商业转售”许可，不属于 OSI 标准开源。官方版本免费，禁止倒卖、强制捐赠或冒充；捐赠完全自愿。第三方组件遵循自身许可；游戏派生音频和商标不因本项目发布获得新授权，公开发行前需核实相应权利。详见仓库 `LICENSE.txt` 和 `legal/`。

## English

An unofficial DualSense enhancement for the PC game *Onimusha: Way of the Sword*, providing event-driven haptics, adaptive triggers, controller audio and a Windows manager.

### Included in 2.3

- One Simplified Chinese/English installer and a saved language switch in the manager.
- Activation keys, machine binding and the ten-minute trial removed; no licensing server or issuer required.
- DualSense USB/Bluetooth haptics, triggers and audio; Edge USB retained, Edge Bluetooth awaiting hardware validation.
- Optional Alipay/WeChat donations and notices against resale and impersonation. Donations unlock no features.
- Steam path normalization prevents slash/case variants of one installation from being counted as separate games. Multiple real installations still require selection.
- The two full-payment-image buttons and dialogs are removed; QR codes remain on the support page.

Maintenance builds reused the 2.3 filename; identify this build with the accompanying SHA-256 list. The installer retains the previously completed final binaries. This source archive adds bilingual documentation, screenshots and release notes without changing runtime code.

### Files and setup

| File | Description |
| --- | --- |
| `Onimusha-PS5-Adapter-Setup-2.3.exe` | Windows x64 bilingual installer with runtime and existing payload |
| `Onimusha-PS5-Adapter-Source-2.3.zip` | Reviewable source, scripts, dependencies and assets |
| `SHA256SUMS-2.3.txt` | Checksums for both packages |

Close the game before setup. If discovery fails, select the folder containing `OnimushaWotS.exe` and choose **Install / Repair**. Keep **Enable Steam Input** selected in Steam controller settings. [Full guide](USER_GUIDE_2.3_en-US.md)

### Validation and limitations

Earlier build, offline regression and UI checks are in [RELEASE_QA_2.3.md](RELEASE_QA_2.3.md), in Chinese. GitHub preparation separately checks source manifests, package hashes, uploaded assets and draft state; it does not repeat a complete game or hardware matrix.

Clean-machine installation coverage, Edge Bluetooth and long sessions across devices remain unverified. USB/Bluetooth may differ in quality, latency and experience. This build is not code signed.

### License and support

Original code is source available with commercial resale prohibited, not OSI-approved open source. The official release is free; resale, mandatory donations and impersonation are prohibited. Donations are optional. Third-party licenses remain independent. Game-derived audio and trademarks receive no new authorization; verify relevant rights before public distribution. See `LICENSE.txt` and `legal/`.
