//
// Created by xu.bw on 2026/6/6.
// registry.h
#ifndef REGISTRY_H
#define REGISTRY_H

#include "std.hpp"
#include "struct.hpp"
#include "coding.hpp"

class Registry {
public:
    // 获取所有已安装软件
    static std::vector<SoftwareInfo> getAllInstalledSoftware();

    // 枚举当前进程名快照（小写、去扩展名），供并行 registryInit 只读复用，
    // 避免每个软件在后台线程各自 CreateToolhelp32Snapshot 一次（几百个软件可省下数百次快照）。
    static std::vector<std::wstring> snapshotRunningProcesses();

    // 执行卸载
    static bool uninstallSoftware(const SoftwareInfo& software);

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
