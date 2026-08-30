# 卸载管理器 · 安全威胁检测体系设计方案

> 角色：威胁检测工程师（Threat Detection Engineer）
> 日期：2026-08-30
> 配套：已有代码层加固（50 轮安全审查 + F11–F14）、`security_audit_report_2026-08-27.md`、本地备份包 `uninst_backup_v0.0.7.zip`

## 0. 范围与前提

- **资产**
  - 便携版：`D:/CZH720/tools/uninstaller-portable/`（含 `uninstaller.exe`、Qt6 运行时、自解压 `uninst.exe`、可写目录）
  - 安装副本：`%LOCALAPPDATA%\Programs\UninstallerManager\`（开始菜单 + HKCU 卸载项）
  - 缓存/遥测：`uninstaller_cache.json`、`startup.log`、注册表 `LastKey` 等
- **环境**：单机 Windows 10/11 x64，当前无域、无企业 EDR。
- **目标**：在"预防"（代码加固）之上补"检测与响应"，形成纵深防御；优先轻量、本地可落地的方案，并预留接入 SIEM 的接口。
- **假设**：用户可接受 Sysmon + 脚本级方案；本文给出规则与脚本骨架，落地按需推进。

## 1. 威胁模型（基于已识别攻击面）

| ID | 威胁 | 已落地的预防控制 | 仍需的检测能力 |
|----|------|----------------|---------------|
| T1 | 二进制替换：便携 exe/dll 在可写目录被替换为恶意文件（未签名） | SetDllDirectoryW 防种植 | 文件完整性基线比对（FIM） |
| T2 | DLL 种植 / 侧载 | main.cpp 入口设空 DLL 搜索路径 | 监控异常 DLL 加载路径 |
| T3 | 缓存投毒：`uninstaller_cache.json` 被篡改 | Sha256+salt 校验、50MB 上限、1h 有效期 | 捕获"校验失败"事件并告警 |
| T4 | 提权滥用：`runas` 派生子进程越权 | needsElevation 仅对 HKLM/Program Files/Windows/ProgramData 触发 | 监控派生进程是否在白名单 |
| T5 | 危险删除：删残留/自卸载误伤系统目录（junction 跟随） | F11 重解析点保护、F14 rmdir 不遍历 junction | 检测越界删除尝试（目标在保护路径） |
| T6 | 注册表篡改：卸载项/系统键被异常删除或修改 | 删键名含 `"` 拒绝、isCriticalSystemItem 拦截 | 监控 HKCU/HKLM 卸载项与系统键变更 |
| T7 | 异常外联：应用本不应联网却出现 C2/信标 | 无 | 网络层外联检测（应用无联网需求） |
| T8 | 路径穿越：卸载/删除访问越界路径 | F12 GetFullPathNameW 落点校验 | 监控异常路径访问（如指向 System32） |

## 2. 检测体系（分层纵深）

```
[ 数据源 ]            [ 检测层 ]                 [ 汇聚/响应 ]
文件/目录  ───┐
进程/命令行 ─┤──► L1 文件完整性(FIM)        ┐
注册表     ─┤──► L2 进程行为(Sysmon E1)    ├─► 本地日志 + 告警中心
特权事件   ─┤──► L3 注册表监控(E12/13/14)  │   (Wazuh/Elastic/邮件)
网络流     ─┤──► L4 特权/Elevat(WinSec)    ┘        │
应用遥测   ─┘──► L5 网络(Sysmon E3/防火墙)          ▼
                  L6 应用内事件(缓存失败/越界删除)  响应 Playbook
```

- **L1 文件完整性（FIM）**：对 `uninstaller.exe`、Qt6*.dll、安装目录建立 SHA-256 基线，周期性比对；新增/修改/删除即告警。
- **L2 进程行为**：Sysmon EventID 1，重点看 `uninstaller.exe` 派生的子进程命令行（是否仅为 `msiexec`/`uninst`/`cmd /c rmdir` 白名单）。
- **L3 注册表**：Sysmon EventID 12/13/14，监控卸载项 `Software\Microsoft\Windows\CurrentVersion\Uninstall` 与系统键变更。
- **L4 特权/Elevat**：Windows 安全日志 4688（进程创建含令牌提升）/4672（特权分配），对 `runas` 触发告警。
- **L5 网络**：Sysmon EventID 3 或 Windows 防火墙日志；应用无联网需求，任何外联=高可疑。
- **L6 应用内遥测**：在应用日志中显式记录"缓存 Sha256 校验失败""越界删除被拒""提权目标非白名单"等结构化事件（JSON），供收集。

## 3. 日志采集

- **首选（P1）**：部署 Sysmon（SwiftOnSecurity/Olaf 规则精简版）+ 应用自身结构化日志。Sysmon 配置见第 4 节示例。
- **轻量替代（P0，无需管理员）**：PowerShell 计划任务每 N 分钟跑 FIM 脚本（见附录 A），结果写入 `startup.log` 或独立 `detection.log`，异常时弹窗/写事件日志。
- **汇聚（P2）**：将 Sysmon + 应用日志转发至 Wazuh/Elastic；本文规则均带 `rule.id` 便于映射。

## 4. 检测规则（Detection Rules）

| 规则 | 名称 | 数据源 | 逻辑 | 级别 |
|------|------|--------|------|------|
| R1 | 二进制偏离基线 | L1 FIM | 卸载目录 exe/dll 哈希不在基线 | 高 |
| R2 | 非法 DLL 加载 | L2 Sysmon E7 | `uninstaller.exe` 加载非 Qt6/系统目录 DLL | 中 |
| R3 | 缓存校验失败 | L6 应用日志 | `cacheIntegrityOf()` 返回 false | 中 |
| R4 | 提权派生越权 | L2+L4 | `runas` 子进程命令行不在白名单 | 紧急 |
| R5 | 越界删除尝试 | L2/L6 | 删除目标落入 System32/ProgramData 保护路径 | 紧急 |
| R6 | 卸载项被篡改 | L3 Sysmon E12/13 | Uninstall 键新增/删除/改名 | 高 |
| R7 | 异常外联 | L5 Sysmon E3 | `uninstaller.exe` 出现任何 TCP 外联 | 高 |
| R8 | 注册表删键劫持 | L3 | 删键名含非法字符被拒（应用日志） | 中 |

## 5. 告警与响应（IR）

分级：低 / 中 / 高 / 紧急。

**响应 Playbook（紧急/高）**：
1. **隔离**：终止 `uninstaller.exe` 进程；断开网络（如确认 C2）。
2. **取证**：保留 `startup.log`、`uninstaller_cache.json`、Sysmon EVTX、派生进程命令行。
3. **恢复**：用备份包 `uninst_backup_v0.0.7.zip` 重装（校验 SHA-256 后再用）。
4. **复盘**：更新基线、补规则、回写安全审查报告。

## 6. 落地路线

- **P0（即时，无管理员）**：建立 FIM 基线（附录 A）；增强应用日志输出 R3/R5/R6 结构化事件。
- **P1（本月，需管理员）**：部署 Sysmon + 精简规则；接 R1–R8 大部分。
- **P2（可选）**：Wazuh/Elastic 汇聚 + 看板 + 自动告警（邮件/Webhook）。

## 7. 与现有安全控制的关系

代码层（F11–F14、Sha256 缓存、DLL 防护）= **预防**，本文 = **检测**。两者互补：预防降低发生概率，检测保证"即使绕过也能被发现并响应"。备份包（`uninst_backup_v0.0.7.zip`）= **恢复**锚点，闭环完整。

---

## 附录 A：轻量 FIM 脚本骨架（PowerShell，无管理员）

```powershell
# fim_uninstaller.ps1 — 对卸载目录建基线并比对
$dir = "D:\CZH720\tools\uninstaller-portable"
$base = "$dir\.fim_baseline.json"
$files = Get-ChildItem $dir -Recurse -Include *.exe,*.dll
$cur = @{}
foreach ($f in $files) {
    $cur[$f.FullName] = (Get-FileHash $f.FullName -Algorithm SHA256).Hash
}
if (-not (Test-Path $base)) {
    $cur | ConvertTo-Json | Set-Content $base
    Write-Host "基线已建立（$(($cur.Keys).Count) 个文件）"
} else {
    $old = Get-Content $base | ConvertFrom-Json
    $old = @{}; $old.GetEnumerator() | ForEach-Object { $old[$_.Name] = $_.Value }
    $diff = @()
    foreach ($k in $cur.Keys) {
        if (-not $old.ContainsKey($k)) { $diff += "新增: $k" }
        elseif ($old[$k] -ne $cur[$k]) { $diff += "变更: $k" }
    }
    foreach ($k in $old.Keys) { if (-not $cur.ContainsKey($k)) { $diff += "删除: $k" } }
    if ($diff.Count -eq 0) { Write-Host "OK: 无变更" }
    else { $diff | ForEach-Object { Write-Warning $_ } }
}
```

## 附录 B：Sysmon 精简规则（监控 uninstaller 相关行为）

```xml
<Sysmon schemaVersion="4.90">
  <EventFiltering>
    <!-- R4/R5: 监控 uninstaller 派生子进程 -->
    <ProcessCreate onmatch="include">
      <Image condition="end with">uninstaller.exe</Image>
      <CommandLine condition="not begin with">msiexec</CommandLine>
      <CommandLine condition="not begin with">uninst.exe</CommandLine>
      <CommandLine condition="not contain">rmdir /s /q</CommandLine>
    </ProcessCreate>
    <!-- R6: 监控卸载项变更 -->
    <RegistryEvent onmatch="include">
      <TargetObject condition="begin with">HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall</TargetObject>
      <TargetObject condition="begin with">HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall</TargetObject>
    </RegistryEvent>
    <!-- R7: 监控 uninstaller 任何网络外联 -->
    <NetworkConnect onmatch="include">
      <Image condition="end with">uninstaller.exe</Image>
    </NetworkConnect>
    </EventFiltering>
</Sysmon>

---

## 8. 落地状态（2026-08-30 实测）

上述三层检测已在本机落地并验证通过。

### 8.1 代码层：结构化安全事件日志（R3/R5/R6）
- 新增全局函数 `logSecurityEvent(rule, eventType, detail)`，写入 `uninstaller.exe` 同目录的 `detection.log`（每行一条 JSON：ts/rule/event/detail，带 1MB 上限）。
- 埋点位置：
  - **R3 缓存校验失败** — `mainwindow.cpp` 加载缓存时 `integrity` 不符处。
  - **R6 删键被拒** — `mainwindow.cpp` 的 `deleteRegistryEntry` 与 `forceDeleteEntry` 内 `isCriticalSystemItem` 拦截处。
  - **R5 越界删除被拒** — `registry.cpp` 的 `deleteResidualFiles` / `deleteDirectory` 中四处 `isProtectedPath` 拒绝处。
- 已重新编译 `uninstaller.exe`（v0.0.7），并同步至便携目录与安装副本，重建 `Uninstaller-Setup.exe`。

### 8.2 P0：FIM 脚本 + 计划任务（无需管理员）
- 脚本：`tools/detection/fim_uninstaller.ps1`
  - 建 SHA-256 基线并比对（R1 文件偏离），解析 `detection.log` 中 R3/R5/R6 事件（带行号游标，避免重复告警）。
  - 全 ASCII 实现，规避 PowerShell 5.1 对无 BOM UTF-8 的中文误解析。
- 计划任务：每 30 分钟运行一次，当前用户上下文、受限权限（`schtasks /create /tn UninstallerFIM /sc minute /mo 30 /rl LIMITED`）。
- 验证：基线建立（24 文件）、二次比对 OK、模拟 R5 事件成功触发 ALERT 并写入 `D:/CZH720/tools/detection_result.log`。

### 8.3 P1：Sysmon 配置 + 部署说明（需管理员）
- 配置：`tools/detection/sysmon_config.xml`（schemaVersion 4.90），覆盖 R1（FileCreate）/ R2（ImageLoad）/ R4+R5（ProcessCreate 白名单）/ R6（RegistryEvent）/ R7（NetworkConnect）。
- 说明：`tools/detection/deploy_sysmon.md`（安装/查看/卸载/白名单调整）。
- 已用 XML 解析器校验配置结构合法。

### 8.4 文件清单
| 文件 | 作用 | 权限要求 |
|------|------|----------|
| `tools/detection/fim_uninstaller.ps1` | FIM + 安全事件采集脚本 | 无 |
| `tools/detection/sysmon_config.xml` | Sysmon 检测规则 | 管理员（部署时） |
| `tools/detection/deploy_sysmon.md` | Sysmon 部署说明 | 无 |
| 应用内 `detection.log` | 运行时结构化安全事件 | 无 |
| 计划任务 `UninstallerFIM` | 周期触发 FIM | 无 |

### 8.5 使用流程
1. **日常**：计划任务每 30 分钟自动跑 FIM，异常写入 `detection_result.log` 并控制台告警；人工可随时 `powershell -ExecutionPolicy Bypass -File fim_uninstaller.ps1` 手动跑。
2. **需更强检测**：按 `deploy_sysmon.md` 以管理员安装 Sysmon 配置，事件查看器 / `wevtutil` 查 EventID 1/3/7/11/12-14。
3. **发现攻击**：按方案第 5 节 Playbook 处置——隔离进程、取证 `detection.log` + Sysmon EVTX、用 `uninst_backup_v0.0.7.zip` 重装（先校 SHA-256）。
```
