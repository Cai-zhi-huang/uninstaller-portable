# 卸载管理器 (Uninstaller Manager)

一款轻量、绿色的 Windows 卸载管理工具，帮助你集中查看本机已安装软件，识别并清理卸载后残留的注册表项与“空壳”项。

> 当前版本：**v0.1.0（正式版）** ｜ 系统要求：Windows 10 / 11（64 位）

[![Release](https://img.shields.io/badge/release-v0.1.0-blue)](https://github.com/Cai-zhi-huang/uninstaller-portable/releases/tag/v0.1.0)
[![License](https://img.shields.io/badge/license-个人学习使用-lightgrey)](https://github.com/Cai-zhi-huang/uninstaller-portable)

---

## 功能特性

- **软件清单**：扫描系统注册表（`HKLM` / `HKCU` 的 Uninstall 项），列出全部已安装程序，显示名称、版本、发行商、安装大小。
- **磁盘占用排行**：按安装大小取 Top15 画条形图，一眼看清谁最占空间；点击条目直接跳转到列表对应行。
- **安装大小统计**：自动计算软件安装目录占用空间；内置三道上限保护（文件数 / 扫描时间 / 总条目）与事件泵，超大目录、网络盘、含符号链接的目录（如钉钉）都不会卡死界面。同安装目录的多个子组件只计一次，避免 NVIDIA/CUDA 场景大小虚高。
- **全局残留体检**：一键扫描全部残留项对应的磁盘残留（文件夹、开始菜单/桌面快捷方式等），按软件分组列出并支持「一键清理全部」。
- **残留项检测**：智能识别“卸载程序已不存在，但注册表项还在”的残留项；并加入**进程运行护栏**——若软件仍在运行（例如微信），不会误判为残留，避免误删。
- **卸载后自动残留检测**：卸载完成自动扫描该软件残留并弹出清理框；批量卸载后提供合并的「一键清理全部」。
- **卸载前系统还原点**：卸载（含批量）前自动创建还原点，失败静默跳过、不阻断卸载。
- **空壳项识别**：识别仅有版本号、无任何有效路径的空壳注册表项。
- **一键卸载**：支持 MSI 与 EXE 卸载程序，自动处理含空格路径，并在需要时请求 UAC 提权。
- **打开文件位置**：在资源管理器中直接定位并选中软件主程序。
- **删除残留注册表项**：对确认真实残留的项，可删除其注册表项（操作 `HKLM` 时自动提权，并有二次确认）。
- **强制删除条目**：对卸载命令失效的顽固软件，删除注册表项 + 磁盘残留 + 快捷方式（带系统路径护栏，跳过受保护目录）。
- **搜索高亮 + 列拖拽记忆**：搜索命中的名称片段黄底高亮；表头可拖拽重排，列顺序与列宽自动记忆。
- **状态筛选**：全部 / 正常 / 运行中 / 残留 四态过滤，可与搜索词叠加。
- **系统托盘常驻**：关闭按钮最小化到托盘，双击恢复，不易误关。
- **操作历史**：卸载 / 删除残留 / 强制删除等操作自动记录，可查看并导出 CSV / TXT。
- **导出软件清单**：导出全部或仅选中行（CSV / HTML / TSV，含 UTF-8 BOM）。
- **自定义背景**：支持铺满 / 居中 / 平铺、模糊、跟随系统桌面壁纸。
- **开机自启动**：可选写入用户级 HKCU Run，无需管理员权限。
- **启动更新弹窗**：每个大版本首次打开即展示本次更新内容，可勾选“不再提示此版本”。

---

## 下载

- **安装版（约 65 MB）**：[Uninstaller-Setup.exe](https://github.com/Cai-zhi-huang/uninstaller-portable/releases/download/v0.1.0/Uninstaller-Setup.exe) —— 双击即装，自动创建开始菜单与桌面快捷方式
- **便携版（约 25 MB）**：[uninstaller-portable.zip](https://github.com/Cai-zhi-huang/uninstaller-portable/releases/download/v0.1.0/uninstaller-portable.zip) —— 解压后双击 `uninstaller.exe` 即可运行
- **GitHub Release 页面**：[卸载管理器 v0.1.0 (正式版)](https://github.com/Cai-zhi-huang/uninstaller-portable/releases/tag/v0.1.0)

解压后双击 `uninstaller.exe` 即可运行，**无需安装、无外部依赖**（Qt 运行库已随附）。

## 下载与运行

本仓库（及上面的 Release 附件）即为**便携版**发布包：下载后解压，直接双击运行 `uninstaller.exe` 即可，**无需安装、无外部依赖**（Qt 运行库已随附）。

---

## 使用说明

1. 启动后自动扫描并列出已安装软件。
2. 工具栏可搜索软件；状态列会标记残留项（红色）与运行中（蓝色），支持「仅显示残留项」与四态筛选。
3. 右键任意软件可：卸载、扫描残留、查看详情、打开文件位置、复制卸载命令、在注册表中定位、删除注册表项、强制删除、导出选中软件。
4. 「操作」菜单 ▸ **扫描所有残留** 可一次巡检全机残留并批量清理；「视图」菜单 ▸ **占用排行** 查看磁盘占用 Top15。
5. 「本程序」菜单 ▸ 开机自启动 / 查看操作历史；关闭窗口默认最小化到托盘，双击托盘图标恢复。
6. 点击「刷新列表」可重新扫描注册表。

---

## 文件说明

| 文件 / 目录 | 说明 |
| --- | --- |
| `uninstaller.exe` | 主程序 |
| `uninst.exe` | 安装器自带的卸载桩（通过安装器安装后的卸载入口） |
| `Qt6*.dll` | Qt 6 核心运行库（`Qt6Concurrent.dll` 必需，v0.0.6 起使用后台扫描） |
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
- 「强制删除此条目」会绕过残留检测直接删除注册表项与磁盘残留，请谨慎使用。
- 本软件按“正式版”提供，作者不对使用造成的任何损失负责。

---

## 更新记录

完整改动记录见 [CHANGELOG.md](CHANGELOG.md) 与[博客发布文章](https://cai-zhi-huang.github.io/blog/posts/2026-10-06-uninstaller-v010/)。

## 许可证

仅供个人学习与使用。
