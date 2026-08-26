// uninst.cpp —— 独立的纯 Win32 卸载桩（无 Qt 依赖，静态链接）
//
// 由安装器随 App 一同释放到安装目录，并注册为 UninstallString。
// 因为它不加载 Qt DLL，删除整个安装目录时不会因 Qt 而自锁；
// 但它自身位于安装目录内，直接删除会“删除正在运行的自己”而失败，
// 因此先删除注册表/快捷方式，再启动一个 PowerShell 在“本进程退出后”删除安装目录。

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>

// 返回 System32 下指定系统二进制的完整路径，避免仅用文件名时应用目录被写入恶意同名文件而被种植执行（F4）。
static std::wstring System32Binary(const wchar_t* name) {
    wchar_t sys[MAX_PATH] = {0};
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring dir = (n && n < MAX_PATH) ? std::wstring(sys) : std::wstring(L"C:\\Windows\\System32");
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    return dir + name;
}

static std::wstring SelfDir() {
    wchar_t mod[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, mod, (DWORD)_countof(mod));
    std::wstring s = mod;
    size_t p = s.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? L"" : s.substr(0, p);
}

static std::wstring InstallDirFromReg() {
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\UninstallerManager";
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return L"";
    wchar_t buf[1024] = {}; DWORD sz = sizeof(buf);
    std::wstring r;
    if (RegQueryValueExW(hk, L"InstallLocation", 0, nullptr, (BYTE*)buf, &sz) == ERROR_SUCCESS)
        r = buf;
    RegCloseKey(hk);
    return r;
}

static void KillApp() {
    // 结束可能正在运行的卸载管理器主程序，释放其占用的 Qt DLL 锁。
    // 用 SEE_MASK_NOCLOSEPROCESS + WaitForSingleObject 同步等待结束，确保后续删除目录时它已退出。
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpFile = System32Binary(L"taskkill.exe").c_str();
    sei.lpParameters = L"/F /IM uninstaller.exe";
    sei.nShow = SW_HIDE;
    if (ShellExecuteExW(&sei) && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 5000);
        CloseHandle(sei.hProcess);
    }
}

static void RemoveShortcut() {
    wchar_t startm[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_STARTMENU, nullptr, 0, startm))) {
        std::wstring lnk = std::wstring(startm) + L"\\Programs\\卸载管理器.lnk";
        DeleteFileW(lnk.c_str());
    }
}

static void RemoveReg() {
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\UninstallerManager");
}

static void DoUninstall(const std::wstring& dir) {
    KillApp();
    RemoveShortcut();
    RemoveReg();
    // 延迟删除安装目录：待本进程退出后再删，避免“删除正在运行的自己”自锁。
    DWORD pid = GetCurrentProcessId();
    // F14：删除用 cmd /c rmdir /s /q（而非 PowerShell Remove-Item -Recurse）。
    // 关键差异：rmdir /s 不会“跟随”目录内的 junction/符号链接进入目标目录递归删除，
    // 因此即便安装目录内被植入指向 C:\Windows\System32 的 junction，也只会删掉该 junction 链接本身，
    // 不会误删系统目录。cmd.exe 同样走 System32 完整路径（F4）。
    std::wstring cmdPath = System32Binary(L"cmd.exe");
    // dir 内若含双引号，写成 ""（cmd 字面量转义）；正常安装路径不含引号。
    std::wstring safeDir = dir;
    for (size_t p = 0; (p = safeDir.find(L"\"")) != std::wstring::npos; ) {
        safeDir.replace(p, 1, L"\"\"");
        p += 2;
    }
    std::wstring psCmd = L"Wait-Process -Id " + std::to_wstring(pid) +
        L"; & '" + cmdPath + L"' /c rmdir /s /q \"" + safeDir + L"\"";
    std::wstring params = L"-NoProfile -WindowStyle Hidden -Command \"" + psCmd + L"\"";
    // 用 System32 下的完整 powershell.exe 路径，避免应用目录被种植同名二进制（F4）。
    std::wstring psPath = System32Binary(L"WindowsPowerShell\\v1.0\\powershell.exe");
    ShellExecuteW(nullptr, L"open", psPath.c_str(), params.c_str(), nullptr, SW_HIDE);
    MessageBoxW(nullptr, L"已卸载「卸载管理器」。", L"卸载完成", MB_ICONINFORMATION);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    std::wstring dir = InstallDirFromReg();
    if (dir.empty()) dir = SelfDir();
    DoUninstall(dir);
    return 0;
}
