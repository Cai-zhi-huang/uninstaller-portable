//
// Created by xu.bw on 2026/6/6.
// registry.h
#ifndef REGISTRY_H
#define REGISTRY_H

#include "std.hpp"
#include "struct.hpp"
#include "coding.hpp"

// 卸载结果三态：区分「成功 / 失败 / 用户取消」，避免把用户主动取消（如拒绝 UAC 提权、
// 在 MSI/EXE 向导里点取消）误报成「卸载失败」，从而误导用户、并错误地让列表不刷新。
enum class UninstallResult {
    Success,   // 卸载完成（含 MSI 成功但需重启 3010/1641）
    Failed,    // 启动失败或卸载过程出错
    Canceled   // 用户取消（UAC 提权被拒 / MSI 向导取消）
};

class Registry {
public:
    // 获取所有已安装软件
    static std::vector<SoftwareInfo> getAllInstalledSoftware();

    // 大小去重：NVIDIA Installer2 等安装器给几十个子组件写同一个 InstallLocation，
    // 且注册表 EstimatedSize 全为 0，registryInit 里每个条目各自整目录扫描 →
    // 同一份目录被重复计入几十次（CUDA v11.6 的 30+ 个子组件条目各显示 2.8 GB，
    // 列表总大小严重虚高）。必须在 registryInit 全部完成之后调用（registryInit
    // 会按 EstimatedSize==0 重扫目录并覆盖 getAllInstalledSoftware 阶段的清零）。
    // 仅对「大小来自目录扫描（EstimatedSize 缺失/为 0）」的条目按归一化
    // InstallLocation 去重：同一物理目录只保留第一条的大小，其余清零；
    // 注册表自带大小的条目不受影响。
    static void dedupeSharedLocationSizes(std::vector<SoftwareInfo>& list);

    // 枚举当前进程名快照（小写、去扩展名），供并行 registryInit 只读复用，
    // 避免每个软件在后台线程各自 CreateToolhelp32Snapshot 一次（几百个软件可省下数百次快照）。
    static std::vector<std::wstring> snapshotRunningProcesses();

    // 扫描取消控制：主线程「取消」按钮置位后，findExeOnDisk 的磁盘遍历会在
    // 下一个检查点（≤64 个条目内）退出，QtConcurrent::map 随即快速收尾。
    // resetScanAbort 在每轮扫描开始前调用，清掉上一轮可能残留的取消状态。
    static void requestScanAbort();
    static void resetScanAbort();

    // 执行卸载，返回三态结果（成功 / 失败 / 用户取消）
    static UninstallResult uninstallSoftware(const SoftwareInfo& software);

    // 返回将要执行的卸载命令行（供执行与 UI 预览共用）
    static std::string getUninstallCommand(const SoftwareInfo& software);

    // 扫描残留文件
    static std::vector<std::string> scanResidualFiles(const SoftwareInfo& software, bool force = false);

    // 删除残留文件
    static bool deleteResidualFiles(const std::vector<std::string>& files);

    // 枚举注册软件
    static void enumRegistrySoftware(
        HKEY hive,
        const std::string& subKey,
        std::vector<SoftwareInfo>& softwareList
    );

    // 读取字符串值（自动转换为UTF-8）
    static std::string readString(
        HKEY hive,
        const std::string& path,
        const std::string& valueName
    );

    // 读取DWORD值
    static DWORD readDWord(
        HKEY hive, 
        const std::string& path, 
        const std::string& valueName
    );

    // 检查是否系统组件
    static bool isSystemComponent(
        HKEY hive, 
        const std::string& path
    );

    // 递归删除目录
    static bool deleteDirectory(const std::string& path);

    // 删除软件的注册表卸载项（残留项清理）。HKCU 直接删除；HKLM 访问被拒时提权删除。
    static bool deleteRegistryKey(HKEY hive, const std::string& regPath);
};

#endif // REGISTRY_H
