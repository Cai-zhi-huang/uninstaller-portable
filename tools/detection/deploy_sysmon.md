# Sysmon 部署与说明（P1，需管理员）

本配置把卸载管理器的威胁检测从"轻量脚本"升级到"系统级行为监控"，覆盖威胁模型 T1–T8 中的进程 / DLL / 注册表 / 网络 / 文件落盘维度。

## 前置条件
- Windows 10 / 11 x64
- **管理员权限**的命令提示符（或 PowerShell）
- `Sysmon64.exe`（来自 Microsoft Sysinternals 的 Sysmon 套件，与 `sysmon_config.xml` 同目录）

## 1. 安装并加载配置
```
Sysmon64.exe -i sysmon_config.xml
```
- 首次运行会注册 `Sysmon64` 服务并加载本配置；可能弹出 UAC，需允许。
- 安装成功后会显示 `Sysmon64 installed` 之类提示。

## 2. 验证配置已加载
```
Sysmon64.exe -c
```
应列出当前生效的过滤规则（事件 1/3/7/11/12/13/14）。

## 3. 查看检测到的事件
- **事件查看器**：应用程序和服务日志 → Microsoft → Windows → Sysmon → Operational
- **命令行**（最近 20 条）：
```
wevtutil qe Microsoft-Windows-Sysmon/Operational /c:20 /f:text
```
- **关键事件 ID 映射**：
  | EventID | 含义 | 对应规则 |
  |---------|------|----------|
  | 1  | 进程创建 | R4 / R5（非白名单子进程） |
  | 3  | 网络外联 | R7（异常外联） |
  | 7  | 镜像/DLL 加载 | R2（非法 DLL 侧载） |
  | 11 | 文件创建 | R1（便携目录内自写入，辅助） |
  | 12/13/14 | 注册表 增/值改/删 | R6（卸载项变更） |

## 4. 与 P0 脚本的分工
- **R3（缓存校验失败）/ R8（删键被拒）** 不在 Sysmon 范围内——由应用程序自身写入 `detection.log`，由 `fim_uninstaller.ps1`（P0 计划任务）采集并告警。
- **R1 主手段**是 `fim_uninstaller.ps1` 的哈希基线比对；Sysmon EventID 11 仅作辅助。
- 二者告警可统一汇入 `D:/CZH720/tools/detection_result.log`（P0 脚本）与 Sysmon 事件日志（P1），便于后续 SIEM（P2）聚合。

## 5. 调整白名单
`sysmon_config.xml` 中 `ProcessCreate` 的 `not begin with` / `not contain` 条目即子进程白名单：
- `msiexec`（MSI 卸载）
- `uninst.exe`（自解压残留清理）
- `rmdir /s /q`（自卸载，不遍历 junction）
- `cmd.exe /c`（用于上述 rmdir）

如新增合法子进程类型，在此追加对应 `not ...` 行即可。

## 6. 卸载 Sysmon
```
Sysmon64.exe -u
```
会停止并移除服务与驱动，事件日志保留可后续分析。

## 7. 注意事项
- 本配置仅 `include` 与卸载管理器相关的事件，开销很低，不影响日常使用。
- 事件量大时建议转发至 SIEM（P2：Wazuh / Elastic），规则均已带 `rule` 语义便于映射。
- 未做 Authenticode 签名的 `Sysmon64.exe` 可能被 Defender SmartScreen 拦截，属正常现象。
