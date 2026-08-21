// The struct handle include all base struct.
#pragma once
#include "std.hpp"
#include "global.hpp"


struct filesize_t{
    ld size{0.0};
    std::string format_size{};
    filesize_t(){};
    filesize_t(ld _t);
    void formatN();
    std::string formatS();
    std::string get();
    filesize_t& operator=(ld& _t);
    filesize_t& operator+=(ld& _t);
};

struct SoftwareInfo {
    std::string displayName;      // 软件名称
    std::string displayVersion;   // 版本
    std::string installDate;      // 安装日期
    std::string installLocation;  // 安装位置
    std::string uninstallString;  // 卸载命令
    std::string publisher;        // 发行商
    std::string displayIcon;      // 图标路径（DisplayIcon，可为 path 或 path,-资源ID）
    std::string helpLink;         // 帮助链接
    std::string urlInfoAbout;     // 信息网址
    std::string regPath;          // 注册表路径
    std::string orgPath;          // 全注册表路径
    filesize_t size;              // 大小
    HKEY hive;                    // 键
    bool isRunningTime;       // 是否运行时->1
    bool isSystemComponent;   // 是否系统组件->2
    bool isWindowsInstaller;  // 是否MSI安装->3
    bool isOrphaned;          // 是否残留项（卸载命令指向的文件已不存在）->4

    SoftwareInfo():
    hive(nullptr),
    isRunningTime(false),
    isSystemComponent(false),
    isWindowsInstaller(false),
    isOrphaned(false){}
    SoftwareInfo(std::string reg);
    // 重新读取；runningProcs 为调用方在并行扫描前统一枚举的进程名快照（只读复用，小写、去扩展名）
    void registryInit(const std::vector<std::wstring>& runningProcs);
};