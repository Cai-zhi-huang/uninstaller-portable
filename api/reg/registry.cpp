// registry.cpp
// Created by xu.bw on 2026/6/6.
#include "registry.hpp"
#include "systools.hpp"
#include "place.hpp"
#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <fstream>
#include <tlhelp32.h>

// 前向声明：normalizeCommandLineForArgv 会调用它，而它定义在下方。
static bool splitExeAndParams(const std::wstring& cmd, std::wstring& exePath, std::wstring& params);

// 某些注册表 UninstallString 写成 C:\Program Files\...\uninst.exe /arg（路径无引号），
// 直接传给 CommandLineToArgvW 会按空格拆成 ["C:\\Program", "Files\\...\\uninst.exe", "/arg"]，
// 导致 ShellExecuteExW 报“找不到文件 C:\Program”。这里给 exe 路径补引号。
// 覆盖所有启动器类型（.exe/.bat/.cmd/.ps1/无扩展名等）：先按健壮规则解析出 (exe, 参数)，
// 若 exe 路径含空格则整体补引号，参数段原样保留（含内部引号）。
static std::wstring normalizeCommandLineForArgv(const std::wstring& cmd) {
    if (cmd.empty() || cmd.front() == L'"') return cmd;

    std::wstring lower = cmd;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    if (lower.find(L"msiexec") != std::wstring::npos) return cmd;

    std::wstring exePath, params;
    if (!splitExeAndParams(cmd, exePath, params)) return cmd;

    if (exePath.find(L' ') != std::wstring::npos) {
        std::wstring r = L"\"" + exePath + L"\"";
        if (!params.empty()) r += L" " + params;
        return r;
    }
    if (!params.empty()) return exePath + L" " + params;
    return exePath;
}

// 把卸载命令行拆成 (可执行文件路径, 参数) 两段，参数段【原样保留】(含引号)，
// 不再用 CommandLineToArgvW 拆了再拼（那样会丢掉参数里的引号，导致 rundll32 等命令失败）。
// 对未加引号且含空格的路径，模仿 Windows CreateProcess 的行为：在每段空白边界处探测
// 文件存在性，取最长存在的文件前缀作为可执行文件路径——这样 .bat/.cmd/.ps1/无扩展名 等
// 启动器（如 "C:\Program Files\App\uninst.bat /S"）也能被正确解析为完整路径，
// 而不会像旧逻辑那样按首个空格截断成 "C:\Program"。
static bool splitExeAndParams(const std::wstring& cmd, std::wstring& exePath, std::wstring& params) {
    size_t i = 0;
    while (i < cmd.size() && (cmd[i] == L' ' || cmd[i] == L'\t')) ++i;   // 跳过前导空白
    if (i >= cmd.size()) return false;

    if (cmd[i] == L'"') {
        size_t close = cmd.find(L'"', i + 1);
        if (close == std::wstring::npos) return false;
        exePath = cmd.substr(i + 1, close - i - 1);
        size_t rest = close + 1;
        while (rest < cmd.size() && (cmd[rest] == L' ' || cmd[rest] == L'\t')) ++rest;
        params = cmd.substr(rest);
        return !exePath.empty();
    }

    // 未加引号：按空白分段，逐段探测“该前缀是否为磁盘上存在的文件”，
    // 取最长存在的文件前缀作为可执行文件路径（与 Windows 解析未加引号命令一致）。
    long long bestEnd = -1;
    size_t pos = i;
    while (true) {
        size_t sp = cmd.find(L' ', pos);
        std::wstring candidate = (sp == std::wstring::npos) ? cmd.substr(i) : cmd.substr(i, sp - i);
        while (!candidate.empty() && (candidate.back() == L' ' || candidate.back() == L'\t'))
            candidate.pop_back();
        if (!candidate.empty()) {
            DWORD attr = GetFileAttributesW(candidate.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                bestEnd = static_cast<long long>((sp == std::wstring::npos) ? cmd.size() : sp);
            }
        }
        if (sp == std::wstring::npos) break;
        pos = sp + 1;
    }

    if (bestEnd >= 0) {
        exePath = cmd.substr(i, static_cast<size_t>(bestEnd) - i);
        while (!exePath.empty() && (exePath.back() == L' ' || exePath.back() == L'\t')) exePath.pop_back();
        size_t rest = static_cast<size_t>(bestEnd);
        while (rest < cmd.size() && (cmd[rest] == L' ' || cmd[rest] == L'\t')) ++rest;
        params = cmd.substr(rest);
        return !exePath.empty();
    }

    // 回退：首个空白前的 token（仅当 exe 文件不存在于磁盘时，如已删除的卸载器）。
    size_t sp = cmd.find(L' ', i);
    exePath = (sp == std::wstring::npos) ? cmd.substr(i) : cmd.substr(i, sp - i);
    while (!exePath.empty() && (exePath.back() == L' ' || exePath.back() == L'\t')) exePath.pop_back();
    size_t rest = (sp == std::wstring::npos) ? cmd.size() : sp + 1;
    while (rest < cmd.size() && (cmd[rest] == L' ' || cmd[rest] == L'\t')) ++rest;
    params = cmd.substr(rest);
    return !exePath.empty();
}

std::vector<SoftwareInfo> Registry::getAllInstalledSoftware() {
    std::vector<SoftwareInfo> softwareList;
    // 使用 place.hpp 中定义的路径
    for (const auto& [hive, path] : registryPaths) {
        enumRegistrySoftware(hive, path, softwareList);
    }
    
    // 排序并过滤空名称
    softwareList.erase(
        std::remove_if(softwareList.begin(), softwareList.end(), 
            [](const SoftwareInfo& s) { return s.displayName.empty(); }), 
        softwareList.end()
    );

    std::sort(softwareList.begin(), softwareList.end(), [](const SoftwareInfo& a, const SoftwareInfo& b) {
        return a.displayName < b.displayName;
    });
    return softwareList;
}

void Registry::enumRegistrySoftware(
    HKEY hive,
    const std::string& subKey, 
    std::vector<SoftwareInfo>& softwareList
) {
    // 1. 转换为宽字符路径以支持所有语言
    std::wstring wSubKey = utf8ToWide(subKey);
    HKEY hKey;
    
    // 2. 使用宽字符 API (RegOpenKeyExW)
    if (RegOpenKeyExW(hive, wSubKey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return;
    }

    DWORD index = 0;
    wchar_t keyName[512]; // 增大缓冲区
    while (true) {
        DWORD keyNameSize = 512;
        // 读取子键名称
        LONG result = RegEnumKeyExW(hKey, index, keyName, &keyNameSize, nullptr, nullptr, nullptr, nullptr);
        if (result != ERROR_SUCCESS) {
            break;
        }
        
        // 构建完整路径 (Wide -> UTF8)，并加上 hive 前缀，
        // 否则 SoftwareInfo 构造函数会把 HKCU 下的软件错当成 HKLM。
        std::wstring wFullPath = wSubKey + L"\\" + std::wstring(keyName, keyNameSize);
        std::string fullPath = wideToUtf8(wFullPath);
        std::string hivePrefix;
        if (hive == HKEY_CURRENT_USER) {
            hivePrefix = "HKEY_CURRENT_USER\\";
        } else if (hive == HKEY_LOCAL_MACHINE) {
            hivePrefix = "HKEY_LOCAL_MACHINE\\";
        }
        softwareList.emplace_back(hivePrefix + fullPath);
        index++;
    }
    RegCloseKey(hKey);
}

// 去掉“一对引号包裹的纯路径”首尾的双引号（注册表中带空格的路径常写成 "C:\\Path" 形式）。
// 仅当字符串内部不再含引号时才去引号，避免破坏“带引号的 exe 路径 + 参数”这类完整命令
// （如卸载字符串 "C:\\A B\\u.exe" -c "D:\\x y"），否则会丢引号、命令被毁。
static std::string trimQuotes(const std::string& s) {
    if (s.size() < 2) return s;
    if (s.front() == '"' && s.back() == '"') {
        // 仅当内部没有其他引号时才去掉外层引号（即整个字符串被一对引号完整包围）。
        // 注意：s.find('"', 1) 会找到末尾的闭合引号，所以应判断其位置是否为 size-1。
        if (s.find('"', 1) == s.size() - 1) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}

std::string Registry::readString(
    HKEY hive,
    const std::string& path,
    const std::string& valueName
) {
    std::wstring wPath = utf8ToWide(path);
    std::wstring wValueName = utf8ToWide(valueName);
    
    HKEY hKey;
    if (RegOpenKeyExW(hive, wPath.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return "";
    }

    // 第一次调用获取数据大小
    DWORD dataType = 0;
    DWORD dataSize = 0;
    LONG result = RegQueryValueExW(hKey, wValueName.empty() ? nullptr : wValueName.c_str(), nullptr, &dataType, nullptr, &dataSize);
    
    std::string resultStr;
    if (result == ERROR_SUCCESS && (dataType == REG_SZ || dataType == REG_EXPAND_SZ)) {
        // 分配缓冲区
        std::vector<wchar_t> buffer(dataSize / sizeof(wchar_t) + 1, 0);

        // 第二次调用获取数据
        if (RegQueryValueExW(hKey, wValueName.empty() ? nullptr : wValueName.c_str(), nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(buffer.data()), &dataSize) == ERROR_SUCCESS) {
            if (dataType == REG_EXPAND_SZ) {
                // 展开环境变量（如 %ProgramFiles%），跨机器路径才能正确解析。
                DWORD need = ExpandEnvironmentStringsW(buffer.data(), nullptr, 0);
                if (need > 0) {
                    std::vector<wchar_t> expanded(need, 0);
                    if (ExpandEnvironmentStringsW(buffer.data(), expanded.data(), need) > 0) {
                        resultStr = wideToUtf8(expanded.data());
                    } else {
                        resultStr = wideToUtf8(buffer.data());
                    }
                } else {
                    resultStr = wideToUtf8(buffer.data());
                }
            } else {
                resultStr = wideToUtf8(buffer.data());
            }
        }
    }
    RegCloseKey(hKey);
    return trimQuotes(resultStr);
}

DWORD Registry::readDWord(
    HKEY hive,
    const std::string& path,
    const std::string& valueName
) {
    std::wstring wPath = utf8ToWide(path);
    std::wstring wValueName = utf8ToWide(valueName);
    
    HKEY hKey;
    DWORD result = 0;
    DWORD bufferSize = sizeof(DWORD);
    DWORD type = 0;

    if (RegOpenKeyExW(hive, wPath.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        LONG rc = RegQueryValueExW(hKey, wValueName.empty() ? nullptr : wValueName.c_str(), nullptr, &type, (LPBYTE)&result, &bufferSize);
        RegCloseKey(hKey);
        // 仅当读取成功且类型为 REG_DWORD 时才返回，否则返回 0，避免垃圾数值。
        if (rc != ERROR_SUCCESS || type != REG_DWORD) return 0;
    }
    return result;
}

// 是否为 MSI 安装（注册表标记，或卸载命令里出现 msiexec）
static bool isMsiUninstall(const SoftwareInfo& software, const std::string& lowerCmd) {
    if (software.isWindowsInstaller) return true;
    return lowerCmd.find("msiexec") != std::string::npos;
}

// 构建将要执行的卸载命令行（执行与 UI 预览共用）
std::string Registry::getUninstallCommand(const SoftwareInfo& software) {
    if (software.uninstallString.empty()) return "";

    std::string lower = software.uninstallString;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (isMsiUninstall(software, lower)) {
        size_t pos = software.uninstallString.find('{');
        if (pos != std::string::npos) {
            std::string guid = software.uninstallString.substr(pos);
            guid.erase(std::remove(guid.begin(), guid.end(), '"'), guid.end());
            return "msiexec.exe /x " + guid + " /quiet /norestart";
        }
        // 形如 MsiExec.exe /X{GUID} 但没有花括号时，原样返回
        return software.uninstallString;
    }
    return software.uninstallString;
}

// 卸载是否需要管理员权限：机器级(HKLM)安装，或卸载程序位于系统目录时提权。
static bool needsElevation(const SoftwareInfo& software, const std::wstring& launchFile) {
    if (software.hive == HKEY_LOCAL_MACHINE) return true;
    std::wstring lower = launchFile;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    if (lower.find(L"program files") != std::wstring::npos ||
        lower.find(L"windows\\") != std::wstring::npos ||
        lower.find(L"\\programdata\\") != std::wstring::npos) {
        return true;
    }
    return false;
}

static std::wstring directoryOf(const std::wstring& filePath) {
    size_t pos = filePath.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L"";
    return filePath.substr(0, pos);
}

// 等待进程结束，同时泵入 Qt 事件保持界面响应（模态进度框可正常刷新、不假死）。
static bool waitForProcess(HANDLE hProcess, DWORD timeoutMs) {
    const DWORD chunk = 100; // ms
    DWORD waited = 0;
    while (true) {
        DWORD res = MsgWaitForMultipleObjects(1, &hProcess, FALSE, chunk, QS_ALLINPUT);
        if (res == WAIT_OBJECT_0) return true;           // 进程结束
        if (res == WAIT_OBJECT_0 + 1) {                  // 有界面消息，先处理
            QCoreApplication::processEvents();
            continue;
        }
        if (res == WAIT_TIMEOUT) {
            waited += chunk;
            if (timeoutMs != INFINITE && waited >= timeoutMs) return false;
            QCoreApplication::processEvents();
            continue;
        }
        return false; // 出错
    }
}

bool Registry::uninstallSoftware(const SoftwareInfo& software) {
    if (software.uninstallString.empty()) {
        return false;
    }

    // 统一构建命令行（MSI 走 msiexec /x，普通程序用原始卸载字符串）。
    // 对无引号但含空格的 exe 路径先补引号，防止 CommandLineToArgvW 拆错。
    std::wstring wCmd = normalizeCommandLineForArgv(utf8ToWide(getUninstallCommand(software)));

    // 解析命令行：拆成 (exe 路径, 参数) 两段，参数段原样保留引号。
    std::wstring appPath, parameters;
    if (!splitExeAndParams(wCmd, appPath, parameters)) return false;

    bool elevate = needsElevation(software, appPath);
    std::wstring dir = directoryOf(appPath);

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = NULL;
    sei.lpVerb = elevate ? L"runas" : L"open"; // 需要管理员权限时触发 UAC 提权
    sei.lpFile = appPath.c_str();
    sei.lpParameters = parameters.empty() ? NULL : parameters.c_str();
    sei.lpDirectory = dir.empty() ? NULL : dir.c_str();
    sei.nShow = SW_NORMAL; // 显示卸载向导，让用户可见可操作

    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        return false;
    }

    // MSI 静默卸载通常很快结束；普通 EXE 卸载向导会一直等待用户操作（用 INFINITE）。
    bool done = waitForProcess(sei.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(sei.hProcess, &exitCode);
    CloseHandle(sei.hProcess);

    std::string lowerCmd = getUninstallCommand(software);
    std::transform(lowerCmd.begin(), lowerCmd.end(), lowerCmd.begin(), ::tolower);
    if (lowerCmd.find("msiexec") != std::string::npos) {
        // MSI 退出码：0=成功，3010/1641=成功但需重启
        return exitCode == 0 || exitCode == 3010 || exitCode == 1641;
    }
    // 普通 EXE 退出码语义各异，进程正常结束即视为已执行
    return done;
}

// 判断字符串是否为 GUID 形式，支持 {GUID} 和纯 GUID 两种写法。
static bool looksLikeGuid(const std::string& s) {
    if (s.size() < 36) return false;
    std::string t = s;
    if (t.front() == '{') {
        size_t close = t.find('}');
        if (close == std::string::npos) return false;
        t = t.substr(1, close - 1);
    }
    if (t.size() != 36) return false;
    for (size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

// 从 Windows Update 键名（如 {GUID}.KB2151757）中提取 KB 号，若无则返回空。
static std::string extractKbFromKey(const std::string& key) {
    size_t pos = key.find(".KB");
    if (pos != std::string::npos) {
        std::string kb = key.substr(pos + 1);
        // KB 号只保留 KB 及后续数字/字母
        size_t end = 0;
        while (end < kb.size() && (std::isalnum(static_cast<unsigned char>(kb[end])) || kb[end] == '.' || kb[end] == '_')) {
            ++end;
        }
        return kb.substr(0, end);
    }
    return "";
}

// 若 DisplayName 是占位符（如 ${arpDisplayName}）或为空，尝试回退到 QuietDisplayName、BundleName、
// ParentDisplayName、InstallLocation 目录名，或从注册表子键名提取 KB/Display 信息，
// 确保列表里不会显示一堆 GUID 或 ${xxx}。
static std::string resolveDisplayName(HKEY hive, const std::string& regPath, const std::string& fallbackKey) {
    auto usable = [](const std::string& s) {
        return !s.empty() && s.find("${") == std::string::npos;
    };

    std::string name = Registry::readString(hive, regPath, "DisplayName");
    if (usable(name)) return name;

    for (const char* key : { "QuietDisplayName", "BundleName", "ParentDisplayName" }) {
        std::string alt = Registry::readString(hive, regPath, key);
        if (usable(alt)) return alt;
    }

    std::string loc = Registry::readString(hive, regPath, "InstallLocation");
    if (!loc.empty()) {
        try {
            fs::path p(loc);
            std::string base = p.filename().string();
            if (!base.empty() && base != "/" && base != "\\") return base;
        } catch (...) {}
    }

    std::string publisher = Registry::readString(hive, regPath, "Publisher");
    std::string version = Registry::readString(hive, regPath, "DisplayVersion");
    std::string kb = extractKbFromKey(fallbackKey);

    // {GUID}.KBxxxxxx 这类 Windows Update 更新
    if (!kb.empty()) {
        std::string label = "Windows Update (" + kb + ")";
        if (!version.empty()) label += " " + version;
        if (!publisher.empty()) label += " - " + publisher;
        return label;
    }

    // {GUID}_is1 这类 Inno Setup / 运行库条目
    size_t is1Pos = fallbackKey.find("_is1");
    if (is1Pos != std::string::npos) {
        std::string prefix = fallbackKey.substr(0, is1Pos);
        if (looksLikeGuid(prefix)) {
            std::string label = "System Component";
            if (!publisher.empty()) label += " (" + publisher + ")";
            if (!version.empty()) label += " " + version;
            return label;
        }
        return prefix;
    }

    // {GUID}_Display.xxx 这类显示驱动/组件
    size_t dispPos = fallbackKey.find("_Display.");
    if (dispPos != std::string::npos) {
        std::string suffix = fallbackKey.substr(dispPos + 9);
        std::string label = "Display " + suffix;
        if (!publisher.empty()) label += " (" + publisher + ")";
        return label;
    }

    // 纯 GUID 回退：用 Publisher + 版本组合一个可读名称
    if (looksLikeGuid(fallbackKey)) {
        std::string label;
        if (!publisher.empty()) {
            label = publisher + " Component";
            if (!version.empty()) label += " " + version;
        } else {
            label = "System Component";
            if (!version.empty()) label += " " + version;
        }
        return label;
    }

    if (usable(fallbackKey)) return fallbackKey;
    return name.empty() ? "(Unknown)" : name;
}

// 前向声明：下方 extractDirFromUninstallString 需要复用该函数解析卸载器完整路径。
static std::string extractExeFromUninstallString(const std::string& uninstallStr);

// 当注册表 InstallLocation 为空时，尝试从 UninstallString 解析卸载程序所在目录
// 作为安装目录的合理回退（典型如 Anaconda 未写 InstallLocation，但卸载程序位于安装根目录）。
static std::string extractDirFromUninstallString(const std::string& uninstallStr) {
    if (uninstallStr.empty()) return "";

    // 复用 extractExeFromUninstallString：它会正确处理带空格的无引号路径
    // （如 "C:\Program Files\App\uninst.exe /S"），避免被首个空格截断成 "C:\Program"。
    std::string exe = extractExeFromUninstallString(uninstallStr);
    if (exe.empty()) return "";

    std::wstring wExe = utf8ToWide(exe);
    if (!fs::exists(wExe)) return "";

    size_t sep = exe.find_last_of("\\/");
    if (sep == std::string::npos) return "";

    std::string dir = exe.substr(0, sep);
    std::string lowerDir = dir;
    std::transform(lowerDir.begin(), lowerDir.end(), lowerDir.begin(), ::tolower);
    // MSI 回退到 system32 没有意义，过滤掉
    if (lowerDir.find("windows\\system") != std::string::npos ||
        lowerDir.find("windows\\syswow") != std::string::npos) {
        return "";
    }
    return dir;
}

// 从 UninstallString 提取卸載程序 exe 的完整路径（去掉外层引号与参数）。
// 用于残留项检测：若该 exe 不存在，则注册表项很可能是软件卸载后未清理的残留。
// 注意：未加引号的路径常含空格（如 "C:\Program Files\App\uninst.exe /S" 或
// "C:\Program Files\App\uninst.bat /S"），必须以“已知启动器扩展名”作为可执行文件结束位置，
// 不能被首个空格截断（否则 .bat/.cmd/.ps1 卸载器会被误截成 C:\Program，进而误判残留）。
// 这里【不】依赖文件存在性探测，而是按扩展名定位，因此即便卸载器已被删除也能正确解析路径
// （残留检测正需要“路径正确但文件不存在”这一组合来判断 orphan）。
static std::string extractExeFromUninstallString(const std::string& uninstallStr) {
    if (uninstallStr.empty()) return "";
    std::string s = trimQuotes(uninstallStr);
    if (s.empty()) return "";
    if (s.front() == '"') {
        size_t end = s.find('"', 1);
        return (end != std::string::npos) ? s.substr(1, end - 1) : s.substr(1);
    }

    // 已知启动器扩展名（按扩展名定位结束位置，覆盖 .exe/.bat/.cmd/.ps1 等）。
    static const char* exts[] = {
        ".exe", ".bat", ".cmd", ".com", ".ps1", ".vbs", ".js", ".msi", ".msc"
    };
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    size_t bestPos = std::string::npos;
    size_t bestLen = 0;
    for (const char* e : exts) {
        size_t elen = std::strlen(e);
        size_t p = lower.find(e);
        while (p != std::string::npos) {
            size_t after = p + elen;
            // 扩展名后必须紧跟空白/引号/字符串结尾（避免误匹配路径中间出现的 "exe" 等字样）
            if (after >= s.size() || s[after] == ' ' || s[after] == '\t' || s[after] == '"') {
                if (bestPos == std::string::npos || p > bestPos) { bestPos = p; bestLen = elen; }
            }
            p = lower.find(e, after);
        }
    }
    if (bestPos != std::string::npos) {
        return s.substr(0, bestPos + bestLen);
    }

    // 无已知扩展名（含无扩展名启动器）：回退到首个空白前的 token。
    size_t sp = s.find(' ');
    return (sp == std::string::npos) ? s : s.substr(0, sp);
}

// 取文件所在目录（不含文件名）。
static std::string dirOfExe(const std::string& exePath) {
    size_t pos = exePath.find_last_of("\\/");
    return (pos == std::string::npos) ? std::string() : exePath.substr(0, pos);
}

// 从 DisplayIcon 解析出“主程序 exe”路径（去掉外层引号与 ",n" 图标索引）。
// 仅当 DisplayIcon 指向 .exe 时返回有效路径，指向 .ico/.png 等图标资源则返回空。
// 用于残留项核对：卸载入口失效时，若 DisplayIcon 指向的主程序仍在磁盘，说明软件
// 其实还在用（可能重装到别处），不应误判为残留。
static std::string mainExeFromDisplayIcon(const std::string& displayIcon) {
    if (displayIcon.empty()) return "";
    std::string s = trimQuotes(displayIcon);
    size_t comma = s.find(',');
    if (comma != std::string::npos) s = s.substr(0, comma);
    size_t dot = s.find(".exe");
    if (dot == std::string::npos) dot = s.find(".EXE");
    if (dot == std::string::npos) return "";
    return s.substr(0, dot + 4);
}

// 检查目录（仅直接子项，不递归）是否含有任意 .exe 文件。
// 用于残留判定的“软件仍在使用”兜底护栏：当 DisplayIcon 指向的主程序文件缺失，
// 但安装目录里还存在别的 exe（说明软件可能还在用、只是图标文件被删/重装挪走），
// 则不误判为残留。仅检查一层，避免对 Anaconda 这类大目录递归遍历卡顿。
static bool dirContainsExe(const std::wstring& dir) {
    std::error_code ec;
    if (dir.empty() || !fs::exists(dir, ec)) return false;
    for (auto it = fs::directory_iterator(dir, ec); it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code ec2;
        if (it->is_regular_file(ec2)) {
            std::wstring ext = it->path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (ext == L".exe") return true;
        }
    }
    return false;
}

// 判断某个软件关键字（如 "WeChat"，无扩展名）当前是否有进程在运行。
// 采用“前缀匹配”：进程名（去掉扩展名后）以该关键字开头即视为命中。
// 理由：很多软件进程名与 DisplayIcon 文件名不一致（如微信 DisplayIcon 指向
// WeChat.exe，但实际进程叫 WeChatAppEx），精确匹配会漏；前缀匹配能覆盖这种情形。
// 仅当关键字长度 >= 4 才匹配，避免过短/通用关键字（如 "App"、"run"）误命中无关进程。
// 用于残留判定的“软件正在使用”最强护栏：即使卸载入口失效、主程序文件已被删/挪走，
// 只要该软件的主程序进程仍在跑（如微信：注册表路径失效、WeChat.exe 已不在，但
// WeChatAppEx 进程在运行），就绝不判为残留，避免误删正在使用的软件。
// 基于文件名（不依赖文件路径/存在性）匹配，因此也能覆盖“路径失效但在用”的软件。
static bool isProcessRunning(const std::wstring& key) {
    if (key.empty() || key.size() < 4) return false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring p = pe.szExeFile;
            size_t dot = p.rfind(L'.');
            if (dot != std::wstring::npos) p = p.substr(0, dot);  // 去掉扩展名
            if (p.size() >= key.size() && _wcsnicmp(p.c_str(), key.c_str(), key.size()) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// 诊断日志路径：写到 AppData 本地数据目录（如 C:\Users\x\AppData\Local\uninstaller-portable\），
// 回退到 %TEMP%。这样即使 exe 位于被锁定的 C:\Program Files、无写权限，也不会在每次启动扫描时
// 去碰 exe 目录导致频繁失败重试（之前是写在 exe 同目录，C 盘锁定时每扫一个软件都无效重试一次）。
// 仅在设置了环境变量 UNINST_DBG=1 时才真正写日志，正常启动完全不碰磁盘写。
static QString diagLogPath() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty()) base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (base.isEmpty()) return QString();
    QDir().mkpath(base);
    return base + "/uninstaller_diag.log";
}

// 诊断日志：每次启动重写 uninstaller_diag.log，记录每个软件的残留核对依据，
// 便于排查“残留项没扫描出来”时程序真实运行时的判断（用户可把文件发回分析）。
// 默认关闭（不写），仅当设置 UNINST_DBG=1 时才启用；且日志写到 AppData 而非 exe 目录。
static void logOrphanCheck(const std::string& name, const std::string& uninstallString,
                           bool exeMissing, bool dirGone, bool iconAlive, bool uninstallDirGone, bool orphaned,
                           const filesize_t& size) {
    static bool first = true;
    static const bool enabled = (std::getenv("UNINST_DBG") != nullptr);
    if (!enabled) { first = false; return; }
    std::ios::openmode mode = first ? std::ios::trunc : std::ios::app;
    first = false;
    QString logPath = diagLogPath();
    if (logPath.isEmpty()) return;
    std::ofstream f(logPath.toUtf8().constData(), mode);
    if (!f) return;
    f << "[name=" << name
      << "] [exeMissing=" << (exeMissing ? "Y" : "N")
      << "] [dirGone=" << (dirGone ? "Y" : "N")
      << "] [unDirGone=" << (uninstallDirGone ? "Y" : "N")
      << "] [iconAlive=" << (iconAlive ? "Y" : "N")
      << "] [orphaned=" << (orphaned ? "Y" : "N")
      << "] [size=" << size.format_size << " / " << static_cast<long long>(size.size) << "]"
      << " us=" << uninstallString << "\n";
}

// 受保护（禁止删除）的路径：防止 fs::remove_all 误删系统/用户关键目录造成灾难。
// 规则：
//  1) 盘符根（X:\）一律禁止；
//  2) 任意盘符下的 Windows / Program Files / Program Files (x86) 及其全部子目录一律禁止
//     （不硬编码 C:，系统装在其它盘也能拦住，如 D:\Windows、D:\Program Files）；
//  3) 用户目录根（X:\Users）与程序数据根（X:\ProgramData）本身禁止，
//     但允许其下“应用命名”的残留子目录（如 X:\Users\x\AppData\Local\App、X:\ProgramData\App）。
static bool isProtectedPath(const std::string& p) {
    if (p.empty()) return true;
    std::string s = p;
    for (auto& c : s) if (c == '\\') c = '/';
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // 盘符根：长度 <= 3（如 "c:/"）或形如 "c:" 之类
    if (lower.size() <= 3) return true;
    if (lower[1] == ':' && lower.back() == '/') return true;

    // 取盘符前缀（如 "d:"），在其下匹配系统路径，避免只硬编码 C: 而在系统装在其它盘时漏拦。
    std::string drivePrefix;
    if (lower.size() >= 2 && lower[1] == ':') drivePrefix = lower.substr(0, 2);

    // 这些系统树（任意盘符）及其全部子目录：绝对禁止
    static const char* trees[] = {
        "/windows",
        "/program files",
        "/program files (x86)",
    };
    for (const char* t : trees) {
        std::string full = drivePrefix + t; // 例："d:/windows"
        if (lower == full) return true;
        std::string pref = full + "/";
        if (lower.rfind(pref, 0) == 0) return true;
    }

    // 用户/程序数据根本身禁止（其下应用子目录仍可删）
    if (lower == drivePrefix + "/users" || lower == drivePrefix + "/programdata") return true;

    // 直接位于用户根下的“家目录”本身禁止（防误删整个 X:/Users/x）
    if (!drivePrefix.empty() && lower.rfind(drivePrefix + "/users/", 0) == 0) {
        std::string rest = lower.substr(drivePrefix.size() + 7); // 跳过 "x:/users/"
        if (rest.find('/') == std::string::npos) return true; // 正好 x:/users/<name>
    }

    return false;
}

std::vector<std::string> Registry::scanResidualFiles(const SoftwareInfo& software, bool force)
{
    std::vector<std::string> result;

    // 去重地“存在才加入”；任何受保护（系统/用户关键）路径一律忽略，绝不加入删除清单。
    auto addIfExists = [&](const std::string& p) {
        if (p.empty()) return;
        if (isProtectedPath(p)) return;   // 安全护栏：禁止加入任何受保护路径
        if (std::find(result.begin(), result.end(), p) != result.end()) return;
        std::wstring w = utf8ToWide(p);
        std::error_code ec;
        if (fs::exists(w, ec)) {
            result.push_back(p);
        }
    };

    // “扫描残留文件”只针对真正残留的软件（isOrphaned：已卸载但注册表项仍在的僵尸条目）。
    // 正常安装的软件其磁盘目录都在使用中，不是残留，若把它们也列出来并允许删除，
    // 反而可能误删正在使用的软件数据。故非 orphaned 直接返回空。
    // force=true 时（如“强制删除此条目”）绕过该限制，用户已明确承担误删风险。
    if (!software.isOrphaned && !force) {
        return result;
    }

    // 1) 常见用户 / 系统残留目录（卸载器最常漏删的位置）。
    //    用系统 API 取已知文件夹路径，兼容非 C 盘用户目录 / 域账户（不再硬编码 C:\Users）。
    auto knownFolder = [](int csidl) -> std::string {
        wchar_t buf[MAX_PATH] = { 0 };
        if (SHGetFolderPathW(nullptr, csidl, nullptr, 0, buf) == S_OK) return wideToUtf8(buf);
        return "";
    };
    std::string localApp   = knownFolder(CSIDL_LOCAL_APPDATA);
    std::string roamingApp = knownFolder(CSIDL_APPDATA);
    std::string commonApp  = knownFolder(CSIDL_COMMON_APPDATA);
    std::string startMenu  = knownFolder(CSIDL_COMMON_STARTMENU);

    std::vector<std::string> names;
    names.push_back(software.displayName);
    if (!software.publisher.empty()) names.push_back(software.publisher);
    for (const auto& nm : names) {
        if (nm.empty()) continue;
        if (!localApp.empty()) {
            addIfExists(localApp + "\\" + nm);
            addIfExists(localApp + "\\Programs\\" + nm);
        }
        if (!roamingApp.empty()) addIfExists(roamingApp + "\\" + nm);
        if (!commonApp.empty())  addIfExists(commonApp + "\\" + nm);
        if (!startMenu.empty())  addIfExists(startMenu + "\\" + nm);
    }

    // 2) 安装根目录：优先 InstallLocation；缺失时由卸载程序路径推导（不要求 exe 仍存在）
    std::string rootFromUninstall = dirOfExe(extractExeFromUninstallString(software.uninstallString));
    std::string candidateRoot;
    if (!software.installLocation.empty()) {
        candidateRoot = software.installLocation;
    } else if (!rootFromUninstall.empty()) {
        candidateRoot = rootFromUninstall;
    }

    if (!candidateRoot.empty()) {
        addIfExists(candidateRoot);
    }

    // 注意：旧实现还会把“卸载程序目录的上一层”也加入删除清单，会误将
    // C:\Windows、C:\Program Files、甚至盘符根目录送进 fs::remove_all，造成灾难性误删。
    // 现已彻底移除该探测逻辑，并辅以 isProtectedPath 护栏（见 addIfExists 与 deleteResidualFiles）。

    return result;
}

bool Registry::deleteResidualFiles(const std::vector<std::string>& files)
{
    bool success = true;
    for (const auto& file : files) {
        // 最后一道安全护栏：受保护路径绝不删除（scanResidualFiles 已过滤，这里再防御一次）。
        if (isProtectedPath(file)) {
            success = false;
            continue;
        }
        try {
            std::wstring wPath = utf8ToWide(file);
            if (fs::exists(wPath)) {
                if (fs::is_directory(wPath)) {
                    fs::remove_all(wPath);
                }
                else {
                    fs::remove(wPath);
                }
            }
        }
        catch (const std::exception& e) {
            success = false;
        }
    }
    return success;
}

bool Registry::deleteDirectory(const std::string& path)
{
    // 安全护栏：受保护路径绝不删除（与 scanResidualFiles / deleteResidualFiles 保持一致）。
    if (isProtectedPath(path)) return false;
    try {
        std::wstring wPath = utf8ToWide(path);
        fs::remove_all(wPath);
        return true;
    }
    catch (...) {
        return false;
    }
}

// 以管理员权限删除注册表项（HKLM 访问被拒时使用）。
// 通过 runas 调起 reg.exe 删除，路径以 UTF-16 传入，兼容中文/特殊字符。
static bool deleteRegistryKeyElevated(HKEY hive, const std::string& regPath) {
    QString hiveName = (hive == HKEY_LOCAL_MACHINE) ? "HKLM" : "HKCU";
    std::wstring wFull = utf8ToWide(hiveName.toStdString() + "\\" + regPath);
    std::wstring wParams = L"delete \"" + wFull + L"\" /f";
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = NULL;
    sei.lpVerb = L"runas";          // 触发 UAC 提权
    sei.lpFile = L"reg.exe";
    sei.lpParameters = wParams.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        return false;
    }
    // 必须先等 reg.exe 进程结束，再取退出码；否则刚启动就拿 STILL_ACTIVE(259)，
    // 导致“明明删成功却误报失败”且列表不刷新。
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(sei.hProcess, &exitCode);
    CloseHandle(sei.hProcess);
    return exitCode == 0;
}

bool Registry::deleteRegistryKey(HKEY hive, const std::string& regPath) {
    std::wstring wPath = utf8ToWide(regPath);
    LONG r = RegDeleteTreeW(hive, wPath.c_str());
    if (r == ERROR_SUCCESS) return true;
    // HKLM 项通常需要管理员权限，访问被拒时降级为提权删除。
    if (r == ERROR_ACCESS_DENIED && hive == HKEY_LOCAL_MACHINE) {
        return deleteRegistryKeyElevated(hive, regPath);
    }
    return false;
}

bool Registry::isSystemComponent(
    HKEY hive,
    const std::string& path
){
    DWORD value = readDWord(hive, path, "SystemComponent");
    return value == 1;
}

SoftwareInfo::SoftwareInfo(std::string reg) :
    orgPath(reg),
    regPath(reg),
    isSystemComponent(false),
    isWindowsInstaller(false),
    isRunningTime(false),
    isOrphaned(false) {
    hive = HKEY_LOCAL_MACHINE;
    regPath = reg;
    if (reg.find("HKEY_CURRENT_USER\\") == 0) {
        hive = HKEY_CURRENT_USER;
        regPath = reg.substr(18);
    }
    else if (reg.find("HKEY_LOCAL_MACHINE\\") == 0) {
        hive = HKEY_LOCAL_MACHINE;
        regPath = reg.substr(19);
    }

    // 取注册表子键名作为最后的回退名称
    std::string fallbackKey;
    size_t lastSlash = reg.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        fallbackKey = reg.substr(lastSlash + 1);
    }
    displayName = resolveDisplayName(hive, regPath, fallbackKey);
}

void SoftwareInfo::registryInit() {
    // 读取各项信息 - readString 已通过宽字符 API 返回 UTF-8，直接供 Qt 使用
    this->displayVersion = Registry::readString(hive, regPath, "DisplayVersion");
    this->installDate = Registry::readString(hive, regPath, "InstallDate");
    this->uninstallString = Registry::readString(hive, regPath, "UninstallString");
    this->installLocation = Registry::readString(hive, regPath, "InstallLocation");
    // 注册表未写 InstallLocation 时（典型如 Anaconda / 钉钉），从 UninstallString 解析出
    // 卸载程序所在目录作为安装根目录回退显示。该回退目录可能很大（Anaconda / 钉钉 数万文件），
    // 但其体积计算已由 informat::getsize 加了文件数/时间/总条目数三上限保护，
    // 并在遍历中定期泵 Qt 事件，不会卡死 UI。
    if (this->installLocation.empty() && !this->uninstallString.empty()) {
        std::string fallbackLoc = extractDirFromUninstallString(this->uninstallString);
        if (!fallbackLoc.empty()) {
            this->installLocation = fallbackLoc;
        }
    }
    this->publisher = Registry::readString(hive, regPath, "Publisher");
    this->displayIcon = Registry::readString(hive, regPath, "DisplayIcon");
    this->helpLink = Registry::readString(hive, regPath, "HelpLink");
    this->urlInfoAbout = Registry::readString(hive, regPath, "URLInfoAbout");

    this->size = static_cast<ld>(Registry::readDWord(hive, regPath, "EstimatedSize")) * 1024.0;

    if (this->size.size == 0 && !this->installLocation.empty()) {
        this->size = informat::getsize(this->installLocation);
    }

    // 仅 SystemComponent==1 才算系统组件；WindowsInstaller==1 是普通 MSI 软件，应归入 MSI 桶可见。
    this->isSystemComponent = (Registry::readDWord(hive, regPath, "SystemComponent") == 1);

    this->isWindowsInstaller = (Registry::readDWord(hive, regPath, "WindowsInstaller") == 1);

    // 残留项检测（核对注册表与文件位置是否相符）：
    // 软件若真实安装且可用，注册表声明的“卸载入口(UninstallString 指向的 exe)”
    // 应当还在磁盘上。当“卸载入口已不存在”时：
    //   - 若 DisplayIcon 指向的主程序仍在 → 软件其实在用（卸载路径失效而已），不误判；
    //   - 若主程序也不在，则进一步看安装目录里是否还含有任意 exe：
    //       · 仍含 exe → 软件可能还在用（只是图标文件被删/重装挪走），不误判；
    //       · 不含 exe → 软件确已卸载/无法运行，注册表项即残留，标记出来。
    // 关键：判定残留不要求“安装目录也消失”——用户诉求明确：软件已卸载、只剩
    // 注册表项的就是残留（如 HP 系列：uninstall.exe 与主程序都没了，但更上层
    // 目录还在）。文件夹在 ≠ 软件在用。
    this->isOrphaned = false;
    bool diagExeMissing = false, diagDirGone = false, diagIconAlive = false, diagUninstallDirGone = false;
    if (!this->uninstallString.empty() && !this->isWindowsInstaller) {
        std::string lowerCmd = this->uninstallString;
        std::transform(lowerCmd.begin(), lowerCmd.end(), lowerCmd.begin(), ::tolower);
        if (lowerCmd.find("msiexec") == std::string::npos) {
            std::string exe = extractExeFromUninstallString(this->uninstallString);
            // 仅当 exe 带路径分隔符（指向具体文件）时才校验；
            // 像 rundll32.exe / cmd.exe 这类 PATH 命令不带分隔符，无法可靠判断存在性，跳过。
            if (!exe.empty() && exe.find_first_of("\\/") != std::string::npos) {
                diagExeMissing = !fs::exists(utf8ToWide(exe));
                if (diagExeMissing) {
                    // 卸载入口已不存在：先核对 DisplayIcon 指向的主程序是否还在
                    // （在 → 软件其实在用，只是卸载路径失效，不误判）。
                    std::string iconExe = mainExeFromDisplayIcon(this->displayIcon);
                    if (!iconExe.empty() && fs::exists(utf8ToWide(iconExe))) {
                        diagIconAlive = true;
                    }
                    if (!diagIconAlive) {
                        // 最强护栏：若软件主程序进程当前正在运行，说明软件确在用
                        // （如微信：注册表路径已失效、WeChat.exe 文件已不在，但 WeChatAppEx
                        // 进程仍在跑），绝不判残留，避免误删正在使用的软件。
                        // 进程匹配关键字来源：优先 DisplayIcon 主程序名；若 DisplayIcon 为空
                        // （微信常见），回退用“卸载命令所在目录”的最后一段目录名（如 WeChat），
                        // 前缀匹配即可命中 WeChatAppEx 这类进程。
                        std::string procKey;
                        std::string iconExePath = mainExeFromDisplayIcon(this->displayIcon);
                        if (!iconExePath.empty()) {
                            procKey = iconExePath.substr(iconExePath.find_last_of("\\/") + 1);
                            size_t d = procKey.rfind('.');
                            if (d != std::string::npos) procKey = procKey.substr(0, d);
                        }
                        if (procKey.empty()) {
                            std::string ud = dirOfExe(exe);
                            if (!ud.empty()) {
                                size_t p = ud.find_last_of("\\/");
                                procKey = (p == std::string::npos) ? ud : ud.substr(p + 1);
                            }
                        }
                        if (!procKey.empty() && isProcessRunning(utf8ToWide(procKey))) {
                            diagIconAlive = true;
                        }
                        if (!diagIconAlive) {
                        // 卸载入口缺失 + 主程序(DisplayIcon)也不在、且主程序进程未运行 → 注册表项即残留。
                        // 用户诉求：软件已卸载/无法运行，残留的就是这些“只剩注册表项”的条目，
                        // 不应要求“安装目录也消失”才判残留（文件夹在 ≠ 软件在用，典型如
                        // HP 系列：...\Network PC Fax\uninstall.exe 已删、主程序也没了，
                        // 但其 installLocation 指向的更上层 Hewlett-Packard 目录还在）。
                        // 次级护栏：若安装目录（installLocation，缺失时回退卸载程序所在目录）
                        // 里仍含有任何 exe 文件，说明软件可能还在用（只是 DisplayIcon 图标
                        // 文件被删/重装挪走），则不误判。
                        std::string probeDir = this->installLocation.empty()
                                                    ? dirOfExe(exe)
                                                    : this->installLocation;
                        bool stillHasExe = dirContainsExe(utf8ToWide(probeDir));
                        // 仍记录目录消失信号供诊断
                        std::string uninstallDir = dirOfExe(exe);
                        bool uninstallDirGone = uninstallDir.empty()
                                                    || !fs::exists(utf8ToWide(uninstallDir));
                        bool installDirGone = !this->installLocation.empty()
                                              && !fs::exists(utf8ToWide(this->installLocation));
                        diagDirGone = uninstallDirGone || installDirGone;
                        diagUninstallDirGone = uninstallDirGone;
                        if (!stillHasExe) {
                            this->isOrphaned = true;
                        }
                        }
                    }
                }
            }
        }
    }
    // 无 UninstallString 的项进一步处理：
    // 1) InstallLocation 非空：若声明的安装目录已不存在，或目录存在但里面没有任何文件，视为残留。
    // 2) InstallLocation 为空且 DisplayIcon 也为空：该注册表项没有任何有效路径信息可指向实际
    //    安装的软件（如只剩 DisplayVersion 的空壳 Weixin 4.0.6.33），判为残留。
    // 仍保留 DisplayIcon 主程序存在性护栏，避免误删正在使用但卸载入口异常的软件。
    if (!this->isOrphaned && this->uninstallString.empty() && !this->isWindowsInstaller) {
        if (!this->installLocation.empty()) {
            std::wstring wLoc = utf8ToWide(this->installLocation);
            std::error_code ec;
            bool locExists = fs::exists(wLoc, ec);
            bool locEmpty = true;
            if (locExists) {
                for (auto it = fs::directory_iterator(wLoc, ec);
                     it != fs::directory_iterator() && locEmpty;
                     it.increment(ec)) {
                    if (!ec) locEmpty = false;
                }
            }
            bool iconAlive = false;
            std::string iconExe = mainExeFromDisplayIcon(this->displayIcon);
            if (!iconExe.empty() && fs::exists(utf8ToWide(iconExe))) {
                iconAlive = true;
            }
            if (!iconAlive && (!locExists || locEmpty)) {
                this->isOrphaned = true;
                diagDirGone = !locExists;
            }
        } else if (this->displayIcon.empty() && !this->displayVersion.empty()) {
            this->isOrphaned = true;
        }
    }

    logOrphanCheck(this->displayName, this->uninstallString,
                   diagExeMissing, diagDirGone, diagIconAlive, diagUninstallDirGone, this->isOrphaned,
                   this->size);

    // 将“更新 / 运行库 / Redistributable / .NET”这类系统组件归入“系统组件”分组，
    // 默认列表里就会与用户软件分开显示，避免误卸载运行库。
    if (!this->displayName.empty() && !this->isSystemComponent) {
        std::string lowerName = this->displayName;
        transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

        if (lowerName.find("update") != std::string::npos ||
            lowerName.find("redistributable") != std::string::npos ||
            lowerName.find("runtime") != std::string::npos ||
            lowerName == "microsoft visual c++" ||
            lowerName.find(".net") != std::string::npos) {
            this->isSystemComponent = true;
        }
    }

    // 如果 DisplayName 里没有包含版本号，就把 DisplayVersion 追加到名字后面，
    // 避免像 "Keil μVision4" 这种注册表名和实际版本脱节的情况误导用户。
    if (!this->displayVersion.empty() && !this->displayName.empty()) {
        if (this->displayName.find(this->displayVersion) == std::string::npos &&
            this->displayName.find(this->displayVersion.substr(0, this->displayVersion.find('.'))) == std::string::npos) {
            this->displayName += " (" + this->displayVersion + ")";
        }
    }

}