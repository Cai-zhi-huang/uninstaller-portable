#include "systools.hpp"
#include "coding.hpp"
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <windows.h>
#include <QCoreApplication>

ll informat::getsize(std::string& path){
    if (path.empty()) return 0;

    // 转换为宽字符路径以支持中文
    std::wstring wPath = utf8ToWide(path);

    // 跳过 UNC 网络路径（如 \\server\share）：Windows 对断连/慢速网络共享的
    // 文件访问会进行长达数十秒的重试，直接在主线程造成“卡死/未响应”。
    if (wPath.size() >= 2 && wPath[0] == L'\\' && wPath[1] == L'\\') return 0;
    // 仅扫描本地固定盘（DRIVE_FIXED）；跳过可移动盘/光驱/网络映射盘等，避免长耗时。
    if (wPath.size() >= 3 && wPath[1] == L':') {
        std::wstring root = wPath.substr(0, 3); // 形如 L"D:\\"
        UINT dt = GetDriveTypeW(root.c_str());
        if (dt != DRIVE_FIXED) return 0;
    }

    try {
        if (!fs::exists(wPath)) return 0;
    }
    catch (...) {
        return 0;
    }

    // 超大目录（如 Anaconda / 钉钉 数万文件）若不加限制会长时间阻塞主线程，
    // 导致列表构建期间界面“假死 / 窗口迟迟不出现”。这里用“文件数上限 / 时间预算 /
    // 总条目上限”三道闸限制遍历，并在迭代中定期泵 Qt 事件、跳过目录联接(junction)
    // 与符号链接目录（避免 AppData 等自引用造成的无限递归死循环），确保不会卡死。
    const ll kMaxFiles = 60000;      // 文件数上限
    const ll kTimeBudgetMs = 1000;   // 遍历时间预算（毫秒）
    const ll kMaxEntries = 200000;   // 总条目（文件+目录）上限，防止目录极深/极多导致耗时过长
    const ll kPumpInterval = 64;     // 每 64 个条目泵一次事件并检查时间
    ll totalSize = 0;
    ll fileCount = 0;
    ll entryCount = 0;
    auto start = std::chrono::steady_clock::now();
    try {
        for (auto it = fs::recursive_directory_iterator(wPath,
                 fs::directory_options::skip_permission_denied);
             it != fs::recursive_directory_iterator(); ++it) {
            const auto& entry = *it;
            // 目录联接(junction)/符号链接目录：不递归进入，防止无限遍历死循环。
            if (fs::is_directory(entry.symlink_status())) {
                DWORD attr = GetFileAttributesW(entry.path().c_str());
                if (attr != INVALID_FILE_ATTRIBUTES &&
                    (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    it.disable_recursion_pending();
                }
            }
            ++entryCount;
            if (fs::is_regular_file(entry.symlink_status())) {
                totalSize += fs::file_size(entry.path());
                if (++fileCount >= kMaxFiles) break;
            }
            // 定期泵事件 + 检查时间，避免在单个目录上连续阻塞导致“未响应”
            if ((entryCount & (kPumpInterval - 1)) == 0) {
                if (QCoreApplication::instance()) {
                    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                }
                auto now = std::chrono::steady_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
                if (ms >= kTimeBudgetMs) break;
            }
            if (entryCount >= kMaxEntries) break;
        }
    }
    catch (...) {
        // 忽略访问错误
    }
    return totalSize;
}

// 在带超时的子线程里探测卷/路径是否可访问，避免对断连/慢速网络映射盘（Z:、Y: 等）
// 直接调用 GetVolumeInformationW/GetFileAttributesW 时阻塞数十秒。
// 返回 0=可访问, 1=只读卷, 2=不可访问/无权限/超时。
// 注意：必须用 std::thread + detach 手动超时，不能用 std::async——其 future 析构在
// 任务未结束时（即我们超时提前返回时）会阻塞调用线程，反而造成卡死。
static int probeAccessWithTimeout(const std::wstring& root, const std::wstring& full, int timeoutMs) {
    auto task = std::make_shared<std::packaged_task<int()>>([root, full]() -> int {
        DWORD flags = 0;
        if (!GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr,
                                   &flags, nullptr, 0)) {
            return 2;
        }
        if (flags & FILE_READ_ONLY_VOLUME) return 1;
        DWORD attr = GetFileAttributesW(full.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) return 2;
        return 0;
    });
    std::future<int> fut = task->get_future();
    std::thread th([task]() { (*task)(); });
    auto status = fut.wait_for(std::chrono::milliseconds(timeoutMs));
    if (status == std::future_status::ready) {
        th.join();
        return fut.get();
    }
    // 超时：不 join（会阻塞），detach 让后台线程自行结束；按值捕获 root/full 故无悬空引用。
    th.detach();
    return 2; // 超时 → 视为不可访问，安全跳过（宁可显示默认图标 / 提示，也不要卡住）
}

// 卷访问状态缓存：同一盘符在一次进程内只探测一次。否则对每个软件的图标都调用
// 超时探测，N 个软件 * 800ms 会累积到分钟级，反而造成“加载卡死”的假象。
static std::map<std::wstring, int> g_volumeStateCache;
static std::mutex g_volumeStateMtx;

int informat::volumeAccessState(const std::string& path) {
    if (path.empty()) return 0;
    std::wstring w = utf8ToWide(path);

    // 取根路径（形如 L"D:\\"）。UNC 网络路径（\\server\share）直接视为不可控访问，
    // 不发起任何可能阻塞的网络调用。
    std::wstring root;
    if (w.size() >= 2 && w[1] == L':') {
        size_t sl = w.find_first_of(L"\\/");
        root = (sl != std::wstring::npos) ? w.substr(0, sl + 1) : w.substr(0, 3);
    }
    else if (w.size() >= 2 && w[0] == L'\\' && w[1] == L'\\') {
        return 2; // UNC 网络路径，访问不可控
    }
    else {
        return 0;
    }
    if (root.size() < 3) return 0;

    {
        std::lock_guard<std::mutex> lk(g_volumeStateMtx);
        auto it = g_volumeStateCache.find(root);
        if (it != g_volumeStateCache.end()) return it->second;
    }
    // 对所有盘符（C:、D:、E:、Z: 网络映射盘等）统一走带超时的探测，
    // 锁定/加密(BitLocker 未解锁)/只读/断连的盘都会安全返回非 0，触发跳过。
    int st = probeAccessWithTimeout(root, w, 800);
    {
        std::lock_guard<std::mutex> lk(g_volumeStateMtx);
        g_volumeStateCache[root] = st;
    }
    return st;
}

// 从路径里取出盘符（如 "D:"），用于把提示文案里的“C 盘”泛化成实际出问题的盘。
static QString driveLabelOf(const std::string& f) {
    if (f.size() >= 2 && f[1] == ':') {
        return QString::fromUtf8(f.substr(0, 2).c_str()) + QString::fromUtf8(u8" 盘");
    }
    return QString::fromUtf8(u8"该磁盘");
}

QString informat::diagnoseDeleteFailure(const std::vector<std::string>& files) {
    for (const auto& f : files) {
        int st = volumeAccessState(f);
        QString dv = driveLabelOf(f);
        if (st == 1) {
            return QString::fromUtf8(u8"部分文件位于只读磁盘（") + dv +
                   QString::fromUtf8(u8"可能被锁定 / 写保护），无法删除。请解锁或关闭写保护后重试。");
        }
        if (st == 2) {
            return QString::fromUtf8(u8"部分文件无访问权限（") + dv +
                   QString::fromUtf8(u8"可能被锁定或需要管理员权限）。请以管理员身份运行本程序后重试。");
        }
    }
    return QString::fromUtf8(u8"部分残留文件删除失败，可能正被其它程序占用。请关闭相关程序后重试。");
}
