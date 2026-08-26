# 卸载管理器 (Uninstaller Manager)

一款轻量、绿色的 Windows 卸载管理工具，帮助你集中查看本机已安装软件，识别并清理卸载后残留的注册表项与“空壳”项。

> 当前版本：**v0.0.6 (试用版)** ｜ 系统要求：Windows 10 / 11（64 位）

[![Release](https://img.shields.io/badge/release-v0.0.6-blue)](https://github.com/Cai-zhi-huang/uninstaller-portable/releases/tag/v0.0.6)
[![License](https://img.shields.io/badge/license-个人学习使用-lightgrey)](https://github.com/Cai-zhi-huang/uninstaller-portable)

---

## 功能特性

- **软件清单**：扫描系统注册表（`HKLM` / `HKCU` 的 Uninstall 项），列出全部已安装程序，显示名称、版本、发行商、安装大小。
- **安装大小统计**：自动计算软件安装目录占用空间；内置三道上限保护（文件数 / 扫描时间 / 总条目）与事件泵，超大目录、网络盘、含符号链接的目录（如钉钉）都不会卡死界面。
- **残留项检测**：智能识别“卸载程序已不存在，但注册表项还在”的残留项；并加入**进程运行护栏**——若软件仍在运行（例如微信），不会误判为残留，避免误删。
- **空壳项识别**：识别仅有版本号、无任何有效路径的空壳注册表项。
- **一键卸载**：支持 MSI 与 EXE 卸载程序，自动处理含空格路径，并在需要时请求 UAC 提权。
- **打开文件位置**：在资源管理器中直接定位并选中软件主程序。
- **删除残留注册表项**：对确认真实残留的项，可删除其注册表项（操作 `HKLM` 时自动提权，并有二次确认）。
- **启动更新弹窗**：每个大版本首次打开即展示本次更新内容，可勾选“不再提示此版本”。

---

## 下载

- **GitHub Release（推荐）**：[卸载管理器 v0.0.6 (试用版)](https://github.com/Cai-zhi-huang/uninstaller-portable/releases/tag/v0.0.6)
- **直接下载便携包（zip，约 23 MB）**：[uninstaller-portable.zip](https://github.com/Cai-zhi-huang/uninstaller-portable/releases/download/v0.0.6/uninstaller-portable.zip)

解压后双击 `uninstaller.exe` 即可运行，**无需安装、无外部依赖**（Qt 运行库已随附）。

## 下载与运行

本仓库（及上面的 Release 附件）即为**便携版**发布包：下载后解压，直接双击运行 `uninstaller.exe` 即可，**无需安装、无外部依赖**（Qt 运行库已随附）。

---

## 使用说明

1. 启动后自动扫描并列出已安装软件。
2. 列表「状态」列会标记残留项（红色）；可勾选工具栏「仅显示残留项」进行过滤。
3. 右键任意软件可：卸载、打开文件位置、查看详情、删除残留注册表项。
4. 点击「刷新列表」可重新扫描注册表。

---

## 文件说明

| 文件 / 目录 | 说明 |
| --- | --- |
| `uninstaller.exe` | 主程序 |
| `uninst.exe` | 安装器自带的卸载桩（通过安装器安装后的卸载入口） |
| `Qt6*.dll` | Qt 6 核心运行库 |
| `libc++.dll` / `libunwind.dll` / `libwinpthread-1.dll` | LLVM/MinGW 运行库依赖 |
| `platforms/` `imageformats/` `tls/` `styles/` `iconengines/` `networkinformation/` `generic/` | Qt 插件 |

---

## 从源码构建

- 工具链：Qt 6 + Clang + CMake + Ninja（Windows）
- 本仓库为编译后的便携发布包；完整源码（UI、逻辑、安装器）见主项目仓库。
- 构建示例：

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH="<Qt6>/llvm-mingw_64" \
  -DTOOLCHAIN_BIN_DIR="<llvm-mingw>/bin"
cmake --build build
windeployqt --no-translations --no-opengl build/uninstaller.exe
```

---

## 免责声明

- 删除注册表项与卸载软件属于敏感操作，请确认后再执行；本工具对 `HKLM` 的操作会请求 UAC 提权。
- 本软件按“试用版”提供，作者不对使用造成的任何损失负责。

---

## 许可证

仅供个人学习与使用。
