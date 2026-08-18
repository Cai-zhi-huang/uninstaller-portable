// installer.cpp —— 自解压安装器（纯 Win32，静态链接，零运行时依赖）
//
// 二进制布局：[本程序代码] + [未压缩负载] + [8 字节负载长度 (uint64 LE)]
// 负载 TOC：u32 文件数；每文件：u16 路径长度 + 路径(UTF-8) + u64 文件大小 + 文件内容
//
// 行为：
//   * 双击运行（无参数）= 安装：解压到 %LOCALAPPDATA%\Programs\UninstallerManager，
//     创建开始菜单快捷方式，写入 HKCU 卸载项（出现在“设置-应用”与开始菜单搜索），并启动程序。
//   * 卸载由 App 自身的 /uninstall 处理（UninstallString 指向它）。

#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <shobjidl.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static std::wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w; w.resize((size_t)n);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::vector<uint8_t> ReadFileAll(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    std::vector<uint8_t> buf((size_t)sz.QuadPart);
    DWORD rd = 0;
    if (!buf.empty()) ReadFile(h, buf.data(), (DWORD)buf.size(), &rd, nullptr);
    CloseHandle(h);
    return buf;
}

static uint16_t RdU16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t RdU32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t RdU64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }

struct FileEntry { std::wstring rel; std::vector<uint8_t> data; };

static bool ParsePayload(const std::vector<uint8_t>& blob, std::vector<FileEntry>& files) {
    size_t pos = 0;
    if (blob.size() < 4) return false;
    uint32_t count = RdU32(blob.data() + pos); pos += 4;
    for (uint32_t i = 0; i < count; i++) {
        if (pos + 2 > blob.size()) return false;
        uint16_t plen = RdU16(blob.data() + pos); pos += 2;
        if (pos + plen > blob.size()) return false;
        std::string p((const char*)blob.data() + pos, plen); pos += plen;
        if (pos + 8 > blob.size()) return false;
        uint64_t fsize = RdU64(blob.data() + pos); pos += 8;
        if (pos + fsize > blob.size()) return false;
        FileEntry fe;
        fe.rel = Utf8ToW(p);
        fe.data.assign(blob.begin() + (ptrdiff_t)pos, blob.begin() + (ptrdiff_t)pos + (ptrdiff_t)fsize);
        pos += fsize;
        files.push_back(std::move(fe));
    }
    return true;
}

static void MakeDirs(const std::wstring& dir) {
    std::wstring cur;
    for (size_t i = 0; i < dir.size(); i++) {
        cur.push_back(dir[i]);
        if (dir[i] == L'\\' || dir[i] == L'/') {
            if (cur.size() > 1) CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    CreateDirectoryW(dir.c_str(), nullptr);
}

static bool WriteFileData(const std::wstring& path, const uint8_t* data, size_t len) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    if (len > 0) WriteFile(h, data, (DWORD)len, &wr, nullptr);
    CloseHandle(h);
    return true;
}

static void CreateShortcut(const std::wstring& exePath, const std::wstring& lnkPath, const std::wstring& workDir) {
    CoInitialize(nullptr);
    IShellLinkW* psl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&psl))) {
        psl->SetPath(exePath.c_str());
        psl->SetWorkingDirectory(workDir.c_str());
        psl->SetDescription(L"卸载管理器");
        IPersistFile* ppf = nullptr;
        if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, (void**)&ppf))) {
            ppf->Save(lnkPath.c_str(), TRUE);
            ppf->Release();
        }
        psl->Release();
    }
    CoUninitialize();
}

static void SetRegStr(HKEY hk, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(hk, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

static void WriteUninstallRegistry(const std::wstring& target) {
    HKEY hk;
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\UninstallerManager";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        SetRegStr(hk, L"DisplayName", L"卸载管理器");
        std::wstring ustr = L"\"" + target + L"\\uninst.exe\"";
        SetRegStr(hk, L"UninstallString", ustr);
        SetRegStr(hk, L"QuietUninstallString", ustr);
        SetRegStr(hk, L"DisplayIcon", target + L"\\uninstaller.exe");
        SetRegStr(hk, L"InstallLocation", target);
        SetRegStr(hk, L"Publisher", L"CZH720");
        SetRegStr(hk, L"DisplayVersion", L"0.0.0");
        DWORD one = 1;
        RegSetValueExW(hk, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
        RegSetValueExW(hk, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
        RegCloseKey(hk);
    }
}

static int DoInstall() {
    wchar_t mod[MAX_PATH * 2];
    if (!GetModuleFileNameW(nullptr, mod, (DWORD)_countof(mod))) {
        MessageBoxW(nullptr, L"无法获取安装程序路径。", L"错误", MB_ICONERROR);
        return 1;
    }
    auto file = ReadFileAll(mod);
    if (file.size() < 8) {
        MessageBoxW(nullptr, L"安装包已损坏（缺少负载）。", L"错误", MB_ICONERROR);
        return 1;
    }
    uint64_t payloadSize; memcpy(&payloadSize, file.data() + file.size() - 8, 8);
    // 防整数回绕：payloadSize 接近 UINT64_MAX 时 “+8” 会回绕成小数，导致下方迭代器越界崩溃。
    // 改为分别比较，避免回绕。
    if (payloadSize > file.size() || file.size() - payloadSize < 8) {
        MessageBoxW(nullptr, L"安装包已损坏（负载长度异常）。", L"错误", MB_ICONERROR);
        return 1;
    }
    std::vector<uint8_t> blob(file.begin() + (ptrdiff_t)(file.size() - 8 - (size_t)payloadSize),
                             file.begin() + (ptrdiff_t)(file.size() - 8));
    std::vector<FileEntry> files;
    if (!ParsePayload(blob, files) || files.empty()) {
        MessageBoxW(nullptr, L"解析安装负载失败。", L"错误", MB_ICONERROR);
        return 1;
    }

    wchar_t local[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local);
    std::wstring target = std::wstring(local) + L"\\Programs\\UninstallerManager";
    MakeDirs(target);

    bool allWritten = true;
    for (auto& fe : files) {
        std::wstring dest = target + L"\\" + fe.rel;
        size_t slash = dest.find_last_of(L"\\/");
        if (slash != std::wstring::npos) MakeDirs(dest.substr(0, slash));
        if (!WriteFileData(dest, fe.data.data(), fe.data.size())) allWritten = false;
    }

    wchar_t startm[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_STARTMENU, nullptr, 0, startm);
    std::wstring progDir = std::wstring(startm) + L"\\Programs";
    MakeDirs(progDir);
    CreateShortcut(target + L"\\uninstaller.exe", progDir + L"\\卸载管理器.lnk", target);

    WriteUninstallRegistry(target);

    ShellExecuteW(nullptr, L"open", (target + L"\\uninstaller.exe").c_str(), nullptr, target.c_str(), SW_SHOW);

    MessageBoxW(nullptr,
        L"安装完成。\n\n可在「开始」菜单搜索“卸载管理器”，或在「设置 ▸ 应用 ▸ 已安装的应用」中找到并卸载。",
        L"安装成功", MB_ICONINFORMATION);

    if (!allWritten) {
        MessageBoxW(nullptr,
            L"部分文件写入失败（可能「卸载管理器」正在运行或被其他进程占用）。\n建议先关闭程序，再重新运行本安装程序。",
            L"安装警告", MB_ICONWARNING);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return DoInstall();
}
