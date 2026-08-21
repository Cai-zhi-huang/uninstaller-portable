//
// Created by xubowen on 2026/6/7.
//
#include "mainwindow.hpp"
#include "language.hpp"
#include "version.hpp"
#include <QFileDialog>
#include <QClipboard>
#include <QSettings>
#include <QTextEdit>
#include <QFile>
#include <QTextStream>
#include <QFile>
#include <QFileInfo>
#include <QStyle>
#include <QDir>
#include <QDirIterator>
#include <QRegularExpression>
#include <QCheckBox>
#include <QShortcut>
#include <QMenu>
#include <QEvent>
#include <QContextMenuEvent>
#include <QProcess>
#include <QFormLayout>
#include <QGridLayout>
#include <QPainter>
#include <QImage>
#include <qt_windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <QThread>
#include <QVector>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QCoreApplication>
#include <QtConcurrent>
#include <QFutureWatcher>
#include "systools.hpp"

// 大小列专用表格项：按字节数（UserRole）排序，而不是按 "3.43 GB" / "344.02 MB" 文本排序。
class SizeTableItem : public QTableWidgetItem {
public:
    SizeTableItem(const QString& text, qint64 bytes)
        : QTableWidgetItem(text) {
        setData(Qt::UserRole, bytes);
        setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }

    bool operator<(const QTableWidgetItem& other) const override {
        qint64 a = data(Qt::UserRole).toLongLong();
        qint64 b = other.data(Qt::UserRole).toLongLong();
        return a < b;
    }
};

// 将 HICON 转换为 QImage（Qt 6 已无 QtWinExtras，这里用 GDI 自行转换）。
// 返回 QImage 而非 QPixmap，以便能在非 GUI 线程（后台图标加载线程）安全调用
// —— QPixmap 与 GUI 子系统绑定，在后台线程构造会出问题；QImage 是线程安全的。
static QImage imageFromHICON(HICON hIcon) {
    if (!hIcon) return QImage();

    ICONINFO ii = { 0 };
    if (!GetIconInfo(hIcon, &ii)) return QImage();

    int w = 0, h = 0;
    BITMAP bmp = { 0 };
    if (ii.hbmColor) {
        GetObject(ii.hbmColor, sizeof(BITMAP), &bmp);
        w = bmp.bmWidth;
        h = bmp.bmHeight;
    }
    if ((w <= 0 || h <= 0) && ii.hbmMask) {
        GetObject(ii.hbmMask, sizeof(BITMAP), &bmp);
        w = bmp.bmWidth;
        h = bmp.bmHeight / 2;
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (w <= 0 || h <= 0) {
        w = GetSystemMetrics(SM_CXICON);
        h = GetSystemMetrics(SM_CYICON);
    }

    // CreateCompatibleDC(nullptr) 在 Qt GUI 子系统下可能创建出无法用于 DIB 的 DC，
    // 改为基于屏幕 DC 创建，提高 ExtractIconEx 提取图标后转 QImage 的成功率。
    HDC screenDC = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screenDC);
    ReleaseDC(nullptr, screenDC);
    if (!dc) {
        return QImage();
    }

    BITMAPINFOHEADER bi = { 0 };
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = w;
    bi.biHeight = -h; // 自顶向下
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    RGBQUAD* bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS,
                                    reinterpret_cast<void**>(&bits), nullptr, 0);
    if (!hBmp) {
        DeleteDC(dc);
        return QImage();
    }
    HBITMAP hOld = reinterpret_cast<HBITMAP>(SelectObject(dc, hBmp));
    if (!DrawIconEx(dc, 0, 0, hIcon, w, h, 0, nullptr, DI_NORMAL)) {
        SelectObject(dc, hOld);
        DeleteObject(hBmp);
        DeleteDC(dc);
        return QImage();
    }
    SelectObject(dc, hOld);

    QImage img(w, h, QImage::Format_ARGB32);
    if (bits) {
        // 32-bit DIB（BI_RGB）在内存中的字节顺序为 BGRA，这与 little-endian
        // 下 QImage::Format_ARGB32 的字节顺序一致，直接拷贝即可，不要再 swap R/B。
        memcpy(img.bits(), bits, static_cast<size_t>(w) * h * 4);
    }
    DeleteObject(hBmp);
    DeleteDC(dc);
    return img;
}

// 判断 QImage 是否实际可见（至少有一个像素的 alpha 不太低），
// 用于过滤掉 ExtractIconExW 偶尔返回的透明/无效图标，避免列表出现空白图标。
// 直接在 QImage 上扫描（不依赖 QPixmap），以便后台线程也能安全调用。
static bool imageHasVisiblePixels(const QImage& src) {
    if (src.isNull()) return false;
    QImage img = src.convertToFormat(QImage::Format_ARGB32);
    const int w = img.width(), h = img.height();
    if (w <= 0 || h <= 0) return false;
    const int step = (w * h > 16000) ? 3 : 1;
    for (int y = 0; y < h; y += step) {
        const uchar* line = img.constScanLine(y);
        for (int x = 0; x < w; x += step) {
            if (line[x * 4 + 3] > 10) return true;
        }
    }
    return false;
}

// 判断 QPixmap 是否实际可见（至少有一个像素的 alpha 不太低），
// 用于过滤掉 ExtractIconExW 偶尔返回的透明/无效图标，避免列表出现空白图标。
// 获取一个彩色、确定可见的默认“应用程序”图标，作为无图标时的占位。
// 不依赖系统 SHGetStockIconInfo（它在某些主题/版本下实际返回文件图标），
// 而是自绘一个圆角彩色背景 + 白色应用剪影/名称缩写的图标，保证任何主题下都清晰可见。
// 传入软件名称时会根据名称哈希生成不同背景色，并绘制首字母缩写，使 VC++ 运行库这类无图标条目也能区分。
static QIcon defaultAppIcon(const QString& name = QString()) {
    QPixmap pm(20, 20);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    QColor bg(0x2D, 0x6C, 0xDF); // 默认蓝色
    QString text;
    if (!name.isEmpty()) {
        // 根据名称哈希挑选颜色，让不同软件显示不同底色。
        uint hash = 0;
        for (QChar c : name) {
            hash = hash * 31 + c.unicode();
        }
        static const QColor palette[] = {
            QColor(0x2D, 0x6C, 0xDF), QColor(0x28, 0xA7, 0x45),
            QColor(0xD9, 0x4A, 0x3C), QColor(0xF5, 0xA6, 0x23),
            QColor(0x8E, 0x44, 0xAD), QColor(0x17, 0xA2, 0xB8),
            QColor(0xE0, 0x5A, 0x8A), QColor(0x5D, 0x6D, 0x7E),
            QColor(0x27, 0xAE, 0x60), QColor(0xC0, 0x39, 0x2B),
            QColor(0xE6, 0x74, 0x00), QColor(0x6C, 0x5C, 0xE7)
        };
        bg = palette[hash % (sizeof(palette) / sizeof(palette[0]))];

        // 提取缩写：VC++ 相关统一显示“VC”，其他取前两个字母/数字字符。
        if (name.contains("Visual C++", Qt::CaseInsensitive) ||
            name.contains("VC++", Qt::CaseInsensitive)) {
            text = "VC";
        } else {
            for (QChar c : name) {
                if (c.isLetterOrNumber() && text.length() < 2) {
                    text.append(c.toUpper());
                }
            }
        }
    }

    p.setBrush(bg);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(1, 1, 18, 18), 4, 4);

    if (!text.isEmpty()) {
        p.setPen(Qt::white);
        QFont font = p.font();
        font.setPixelSize(text.length() == 1 ? 12 : 9);
        font.setBold(true);
        p.setFont(font);
        p.drawText(pm.rect(), Qt::AlignCenter, text);
    } else {
        p.setBrush(Qt::white);
        p.drawEllipse(QRectF(6.5, 5, 7, 7));
        p.drawRoundedRect(QRectF(6.5, 13, 7, 3.5), 1.75, 1.75);
    }
    p.end();
    return QIcon(pm);
}

static QString stripQuotes(QString s) {
    if ((s.startsWith('"') && s.endsWith('"')) ||
        (s.startsWith('\'') && s.endsWith('\''))) {
        s = s.mid(1, s.size() - 2);
    }
    return s;
}

// 去掉卸载命令行里 exe 路径两侧的引号（注册表常见格式："C:\\Path\\uninst.exe" /param）。
// 只处理第一个参数（即 exe 路径）的成对引号，保留后续参数中的引号，用于 UI 显示。
static QString stripCommandQuotes(QString s) {
    s = s.trimmed();

    // 情况 1：整个命令首尾被同一对引号包围（如 "C:\\a.exe /S"）
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.mid(1, s.size() - 2);
        return s.trimmed();
    }

    // 情况 2：仅第一个参数（exe 路径）被引号包围，后面还有参数。
    // 例如 "C:\\Path\\uninst.exe" /S
    if (s.startsWith('"')) {
        int close = s.indexOf('"', 1);
        if (close > 1 && (close + 1 == s.size() || s[close + 1].isSpace())) {
            s.remove(close, 1);
            s.remove(0, 1);
        }
    }
    return s;
}

// 从文件提取图标，返回 QImage（线程安全，可在后台线程调用）。
// 返回空 QImage 表示无可用图标。相比 iconFromFile（返回 QIcon），此版本不触碰
// QPixmap / 任何 GUI 资源，因此能安全地放到后台线程执行，避免主线程（进度条）卡顿。
// ② 在调用 ExtractIconExW 前先用 informat::volumeAccessState 判断卷是否可访问：
// 若卷只读/被锁定/无权限，直接跳过，避免 ExtractIconExW 在无法访问的卷上阻塞或报错
// （这正是“加载进度条时访问文件出问题”的典型成因）。
static QImage extractIconImage(const QString& filePath, int index = 0) {
    if (filePath.isEmpty()) return QImage();

    QString lower = filePath.toLower();
    if (lower.endsWith(".ico") || lower.endsWith(".png") ||
        lower.endsWith(".bmp") || lower.endsWith(".jpg") ||
        lower.endsWith(".jpeg") || lower.endsWith(".webp")) {
        // 独立图片文件：直接用 QImage 加载（QImage 线程安全）。
        QImage img(filePath);
        if (!img.isNull() && imageHasVisiblePixels(img)) return img;
        return QImage();
    }

    // 可执行文件：锁定/只读卷直接跳过，避免 ExtractIconExW 阻塞。
    if (informat::volumeAccessState(filePath.toStdString()) != 0) {
        return QImage();
    }

    std::wstring wPath = QDir::toNativeSeparators(filePath).toStdWString();
    // 优先提取大图标再缩放，颜色/细节比 16x16 小图标好很多。
    HICON hIconLarge = nullptr;
    HICON hIcon = nullptr;
    ExtractIconExW(wPath.c_str(), index, &hIconLarge, nullptr, 1);
    if (hIconLarge) {
        hIcon = hIconLarge;
    } else {
        ExtractIconExW(wPath.c_str(), index, nullptr, &hIcon, 1);
    }
    if (hIcon) {
        QImage img = imageFromHICON(hIcon);
        DestroyIcon(hIcon);
        if (imageHasVisiblePixels(img)) return img;
        // 大图标转换失败/不可见时退而尝试小图标（某些图标的资源索引只存了小图标）
        HICON hIconSmall = nullptr;
        ExtractIconExW(wPath.c_str(), index, nullptr, &hIconSmall, 1);
        if (hIconSmall) {
            QImage imgSmall = imageFromHICON(hIconSmall);
            DestroyIcon(hIconSmall);
            if (imageHasVisiblePixels(imgSmall)) return imgSmall;
        }
    }
    return QImage();
}

static QIcon iconFromFile(const QString& filePath, int index = 0) {
    QImage img = extractIconImage(filePath, index);
    if (!img.isNull()) {
        return QIcon(QPixmap::fromImage(img));
    }
    return QIcon();
}

// 从 SoftwareInfo::displayIcon（如 "C:\app.exe" 或 "C:\app.dll,-123"）提取图标。
// DisplayIcon 经常被加上双引号，提取前先去掉，否则 ExtractIconExW 会失败。
// 从 SoftwareInfo::displayIcon 提取图标；缺失或失败时再尝试安装目录 exe、
// 卸载命令 exe，最后回退到确定可见的默认应用图标，避免列表出现空白图标。
static QString extractExePath(const QString& cmd); // 前向声明（定义在文件下方）

// 通过顶层窗口标题查找正在运行的进程 exe 路径，用于注册表路径过期时的图标回退。
static QString findRunningExeByWindowTitle(const QStringList& nameKeys) {
    if (nameKeys.isEmpty()) return QString();

    struct EnumCtx {
        QStringList keys;
        QString result;
    };
    EnumCtx ctx = { nameKeys, QString() };

    auto enumProc = [](HWND hwnd, LPARAM lParam) -> BOOL {
        if (!IsWindowVisible(hwnd)) return TRUE;
        wchar_t title[256] = { 0 };
        GetWindowTextW(hwnd, title, 256);
        if (!title[0]) return TRUE;
        QString t = QString::fromWCharArray(title);
        EnumCtx* c = reinterpret_cast<EnumCtx*>(lParam);
        for (const QString& key : c->keys) {
            if (!key.isEmpty() && t.contains(key, Qt::CaseInsensitive)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid) {
                    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
                    if (hProc) {
                        wchar_t path[MAX_PATH] = { 0 };
                        DWORD size = MAX_PATH;
                        if (QueryFullProcessImageNameW(hProc, 0, path, &size)) {
                            c->result = QString::fromWCharArray(path);
                        }
                        CloseHandle(hProc);
                    }
                }
                return FALSE; // 找到即停止
            }
        }
        return TRUE;
    };

    EnumWindows(enumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

// 从安装目录收集候选 exe 路径（按与软件名/Publisher 的相关度打分后排序）。
// 单独抽出，供 iconForSoftware（GUI 线程）与 softwareIconImage（后台线程）共用，避免逻辑重复。
// 遍历用 DFS + 深度上限（≤2 层）：QDirIterator::Subdirectories 会无视深度上限递归进入全部子目录，
// 一旦 installLocation 是较宽目录，加载进度条期间就会无限制遍历海量文件，触发权限错误/假死
// （“加载进度条时访问文件出问题”）。这里绝不越过 maxDepth 层，且 ② 在入口处对只读/锁定卷直接跳过。
static QList<QString> collectInstallExes(const QString& installLoc,
                                        const QString& displayName,
                                        const QString& publisher) {
    QList<QString> result;
    if (installLoc.isEmpty()) return result;
    // ② 卷只读/被锁定：跳过整个目录遍历，避免 QDir/ExtractIconExW 在无法访问的卷上出问题。
    if (informat::volumeAccessState(installLoc.toStdString()) != 0) return result;

    QDir dir(installLoc);
    if (!dir.exists()) return result;

    struct Candidate {
        QString path;
        QString fileName;
        int depth;
        int score;
    };
    QList<Candidate> candidates;

    const int maxDepth = 2;
    QList<QPair<QString, int>> stack; // (目录路径, 当前深度)
    stack.append({installLoc, 0});
    while (!stack.isEmpty()) {
        QPair<QString, int> cur = stack.takeLast();
        const QString& dirPath = cur.first;
        int depth = cur.second;
        if (depth > maxDepth) continue;
        QDir d(dirPath);
        if (!d.exists()) continue;
        QFileInfoList entries = d.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
        for (const QFileInfo& fi : entries) {
            if (fi.isDir()) {
                if (depth < maxDepth)
                    stack.append({fi.absoluteFilePath(), depth + 1});
            } else if (fi.isFile()) {
                if (fi.fileName().endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
                    QString fp = fi.absoluteFilePath();
                    QString rel = fp.mid(installLoc.length()).replace('\\', '/');
                    int dep = rel.count('/');
                    if (dep <= maxDepth)
                        candidates.append({fp, fi.fileName(), dep, 0});
                }
            }
        }
    }

    auto collectKeys = [](const QString& s) -> QStringList {
        QStringList keys;
        QString base = s;
        int paren = base.indexOf('(');
        if (paren != -1) base = base.left(paren);
        base = base.trimmed().toLower();
        keys << base;
        QRegularExpression re("[a-z0-9]+");
        auto mi = re.globalMatch(base);
        while (mi.hasNext()) {
            QString w = mi.next().captured();
            if (w.length() >= 2) keys << w;
        }
        return keys;
    };

    QStringList softKeys = collectKeys(displayName);
    QStringList pubKeys = collectKeys(publisher);

    for (auto& c : candidates) {
        QString fnBase = QFileInfo(c.fileName).baseName().toLower();
        for (const QString& k : softKeys) {
            if (k.isEmpty()) continue;
            if (fnBase == k) c.score += 200;
            else if (fnBase.contains(k)) c.score += 100;
        }
        for (const QString& k : pubKeys) {
            if (k.isEmpty()) continue;
            if (fnBase == k) c.score += 150;
            else if (fnBase.contains(k)) c.score += 50;
        }
        c.score -= c.depth;
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    for (const auto& c : candidates) result.append(c.path);
    return result;
}

// 线程安全的图标提取：返回 QImage（不构造 QPixmap / 不在后台线程调用 EnumWindows），
// 因此可在后台线程执行。未找到任何图标时返回空 QImage（调用方据此回退到默认图标）。
// allowProcessScan=false 时跳过“通过运行进程窗口找 exe”这一步：EnumWindows 在后台线程
// 调用存在跨线程 SendMessage 死锁风险，故后台加载时不走该分支；GUI 线程的完整版会启用。
// fastMode=true 时只尝试 DisplayIcon 与 UninstallString 指向的单个文件，不扫描安装目录、
// 不枚举进程、不扫 ProgramData，用于主线程填充初始列表，避免进度条卡死。
static QImage softwareIconImage(const SoftwareInfo* sw, bool allowProcessScan, bool fastMode = false) {
    if (!sw) return QImage();

    QString displayName = QString::fromStdString(sw->displayName);

    // 1) 优先用 DisplayIcon（可能带引号 / 资源ID）
    if (!sw->displayIcon.empty()) {
        QString raw = QString::fromStdString(sw->displayIcon).trimmed();
        int comma = raw.lastIndexOf(',');
        int index = 0;
        QString path = raw;
        if (comma != -1) {
            bool ok = false;
            int idx = raw.mid(comma + 1).toInt(&ok);
            if (ok) { index = idx; path = raw.left(comma); }
        }
        path = stripQuotes(path).trimmed();
        QImage img = extractIconImage(path, index);
        if (!img.isNull()) return img;
    }

    // 2) 安装目录里找 exe（DFS ≤2 层，fastMode 跳过以避免扫描大量文件）
    if (!fastMode) {
        QString installLoc = stripQuotes(QString::fromStdString(sw->installLocation)).trimmed();
        for (const QString& exePath : collectInstallExes(installLoc, displayName,
                                                         QString::fromStdString(sw->publisher).trimmed())) {
            QImage img = extractIconImage(exePath, 0);
            if (!img.isNull()) return img;
        }
    }

    // 3) 卸载命令里解析出的 exe
    QString cmd = QString::fromStdString(Registry::getUninstallCommand(*sw));
    QString exe = extractExePath(cmd);
    if (!exe.isEmpty() && QFile::exists(exe)) {
        QImage img = extractIconImage(exe, 0);
        if (!img.isNull()) return img;
    }

    // 4) 运行中的进程 exe（fastMode 与后台线程均跳过，避免 EnumWindows 死锁/卡顿）
    if (!fastMode && allowProcessScan) {
        QStringList nameKeys;
        nameKeys << displayName;
        if (displayName.contains("微信", Qt::CaseInsensitive) ||
            displayName.contains("WeChat", Qt::CaseInsensitive) ||
            displayName.contains("Weixin", Qt::CaseInsensitive)) {
            nameKeys << "微信" << "Weixin" << "WeChat";
        }
        QString runningExe = findRunningExeByWindowTitle(nameKeys);
        if (!runningExe.isEmpty() && QFile::exists(runningExe)) {
            QImage img = extractIconImage(runningExe, 0);
            if (!img.isNull()) return img;
        }
    }

    // 5) C:\ProgramData\Publisher\Product 下的辅助程序（fastMode 跳过）
    if (!fastMode) {
        QString publisher = QString::fromStdString(sw->publisher).trimmed();
        if (!publisher.isEmpty() && !displayName.isEmpty()) {
            QString pubBase = publisher;
            int paren = pubBase.indexOf('(');
            if (paren != -1) pubBase = pubBase.left(paren);
            pubBase = pubBase.trimmed();

            QString nameBase = displayName.trimmed();
            paren = nameBase.indexOf('(');
            if (paren != -1) nameBase = nameBase.left(paren);
            nameBase = nameBase.trimmed();

            QStringList pubNames;
            pubNames << pubBase;
            if (pubBase.contains("腾讯", Qt::CaseInsensitive)) pubNames << "Tencent";

            QStringList prodNames;
            prodNames << nameBase;
            if (nameBase.contains("微信", Qt::CaseInsensitive) ||
                nameBase.contains("WeChat", Qt::CaseInsensitive)) {
                prodNames << "WeChat" << "微信";
            }

            for (const QString& pub : std::as_const(pubNames)) {
                for (const QString& prod : std::as_const(prodNames)) {
                    QString base = "C:/ProgramData/" + pub + "/" + prod;
                    if (informat::volumeAccessState(("C:/ProgramData/" + pub).toStdString()) != 0)
                        continue;
                    QDir pd(base);
                    if (!pd.exists()) continue;
                    QFileInfoList exes = pd.entryInfoList(QStringList() << "*.exe",
                                                          QDir::Files | QDir::NoDotAndDotDot,
                                                          QDir::Name);
                    for (const QFileInfo& fi : exes) {
                        QImage img = extractIconImage(fi.absoluteFilePath(), 0);
                        if (!img.isNull()) return img;
                    }
                }
            }
        }
    }

    return QImage();
}

// 快速版：只在主线程填充初始列表时使用，避免扫描目录/枚举进程导致进度条或界面卡死。
static QIcon iconForSoftwareFast(const SoftwareInfo* sw) {
    if (!sw) return defaultAppIcon();
    QImage img = softwareIconImage(sw, /*allowProcessScan=*/false, /*fastMode=*/true);
    if (!img.isNull()) return QIcon(QPixmap::fromImage(img));
    return defaultAppIcon(QString::fromStdString(sw->displayName));
}

static QIcon iconForSoftware(const SoftwareInfo* sw) {
    if (!sw) return defaultAppIcon();

    QString displayName = QString::fromStdString(sw->displayName);
    QImage img = softwareIconImage(sw, /*allowProcessScan=*/true);
    if (!img.isNull()) {
        return QIcon(QPixmap::fromImage(img));
    }
    // 兜底：确定可见的默认应用图标（按软件名称生成带缩写的彩色图标）
    return defaultAppIcon(displayName);
}

// 深色主题：主界面黑底、浅灰文字，表格/按钮/搜索框均有明显边界，避免内容看不清。
static const char* kAppStyleSheet = R"(
    QMainWindow { background-color: #181a1e; }
    QWidget#centralWidget { background-color: #181a1e; }

    QMenuBar { background-color: #202328; padding: 2px; color: #e0e3e8; }
    QMenuBar::item { padding: 4px 10px; border-radius: 4px; color: #e0e3e8; }
    QMenuBar::item:selected { background-color: #353a43; }
    QMenuBar::item:pressed { background-color: #414752; }
    QMenu { background-color: #202328; color: #e0e3e8; border: 1px solid #3c424d; }
    QMenu::item:selected { background-color: #353a43; }
    QMenu::separator { background-color: #3c424d; height: 1px; margin: 4px 8px; }

    QLabel { font-size: 13px; color: #c9cdd3; }

    QLineEdit {
        border: 1px solid #3c424d;
        border-radius: 6px;
        padding: 6px 10px;
        font-size: 13px;
        background-color: #202328;
        color: #e8eaed;
    }
    QLineEdit:focus { border: 1px solid #5c9ad6; }
    QLineEdit::placeholder { color: #7f8794; }

    QPushButton {
        background-color: #4a90d9;
        color: #ffffff;
        border: none;
        border-radius: 6px;
        padding: 7px 16px;
        font-size: 13px;
    }
    QPushButton:hover { background-color: #5c9fe6; }
    QPushButton:pressed { background-color: #3a7fc7; }

    QPushButton#uninstallBtn { background-color: #d64a3c; }
    QPushButton#uninstallBtn:hover { background-color: #e25b4d; }
    QPushButton#uninstallBtn:pressed { background-color: #b84034; }

    QPushButton#scanBtn,
    QPushButton#detailsBtn,
    QPushButton#refreshBtn { background-color: #535b66; }
    QPushButton#scanBtn:hover,
    QPushButton#detailsBtn:hover,
    QPushButton#refreshBtn:hover { background-color: #616b78; }
    QPushButton#scanBtn:pressed,
    QPushButton#detailsBtn:pressed,
    QPushButton#refreshBtn:pressed { background-color: #454c56; }

    QTableWidget {
        background-color: #202328;
        alternate-background-color: #262a31;
        gridline-color: #323842;
        border: 1px solid #3c424d;
        border-radius: 8px;
        font-size: 13px;
        color: #e0e3e8;
        selection-background-color: #2f4a66;
        selection-color: #ffffff;
    }
    QTableWidget::item {
        padding: 5px 6px;
        color: #e0e3e8;
        background-color: transparent;
    }
    QTableWidget::item:selected {
        background-color: #2f4a66;
        color: #ffffff;
    }
    QHeaderView::section {
        background-color: #2b3038;
        color: #c9cdd3;
        padding: 8px 6px;
        border: none;
        border-bottom: 2px solid #4a525e;
        font-weight: 600;
    }
    QHeaderView::section:horizontal { border-right: 1px solid #3c424d; }

    QStatusBar { background-color: #202328; color: #9da3ad; }
    QStatusBar::item { border: none; }
    QDialog { background-color: #181a1e; }
    QTextEdit {
        background-color: #202328;
        border: 1px solid #3c424d;
        border-radius: 6px;
        color: #e0e3e8;
        font-size: 12px;
    }
    QCheckBox { color: #c9cdd3; }
    QMessageBox { background-color: #202328; }
)";

// 亮色主题：与 kAppStyleSheet 结构对称，白底黑字，按钮/表格保持清晰边界。
static const char* kLightStyleSheet = R"(
    QMainWindow { background-color: #ffffff; }
    QWidget#centralWidget { background-color: #ffffff; }

    QMenuBar { background-color: #f2f2f2; padding: 2px; color: #1a1a1a; }
    QMenuBar::item { padding: 4px 10px; border-radius: 4px; color: #1a1a1a; }
    QMenuBar::item:selected { background-color: #d6d6d6; }
    QMenuBar::item:pressed { background-color: #cccccc; }
    QMenu { background-color: #ffffff; color: #1a1a1a; border: 1px solid #c0c0c0; }
    QMenu::item:selected { background-color: #e6e6e6; }
    QMenu::separator { background-color: #c0c0c0; height: 1px; margin: 4px 8px; }

    QLabel { font-size: 13px; color: #1a1a1a; }

    QLineEdit {
        border: 1px solid #b0b0b0;
        border-radius: 6px;
        padding: 6px 10px;
        font-size: 13px;
        background-color: #ffffff;
        color: #1a1a1a;
    }
    QLineEdit:focus { border: 1px solid #4a90d9; }
    QLineEdit::placeholder { color: #7f8794; }

    QPushButton {
        background-color: #4a90d9;
        color: #ffffff;
        border: none;
        border-radius: 6px;
        padding: 7px 16px;
        font-size: 13px;
    }
    QPushButton:hover { background-color: #5c9fe6; }
    QPushButton:pressed { background-color: #3a7fc7; }

    QPushButton#uninstallBtn { background-color: #d64a3c; }
    QPushButton#uninstallBtn:hover { background-color: #e25b4d; }
    QPushButton#uninstallBtn:pressed { background-color: #b84034; }

    QPushButton#scanBtn,
    QPushButton#detailsBtn,
    QPushButton#refreshBtn { background-color: #e0e0e0; color: #1a1a1a; }
    QPushButton#scanBtn:hover,
    QPushButton#detailsBtn:hover,
    QPushButton#refreshBtn:hover { background-color: #d0d0d0; }
    QPushButton#scanBtn:pressed,
    QPushButton#detailsBtn:pressed,
    QPushButton#refreshBtn:pressed { background-color: #c0c0c0; }

    QTableWidget {
        background-color: #ffffff;
        alternate-background-color: #f5f5f5;
        gridline-color: #d0d0d0;
        border: 1px solid #c0c0c0;
        border-radius: 8px;
        font-size: 13px;
        color: #1a1a1a;
        selection-background-color: #bcdffc;
        selection-color: #1a1a1a;
    }
    QTableWidget::item {
        padding: 5px 6px;
        color: #1a1a1a;
        background-color: transparent;
    }
    QTableWidget::item:selected {
        background-color: #bcdffc;
        color: #1a1a1a;
    }
    QHeaderView::section {
        background-color: #e8e8e8;
        color: #1a1a1a;
        padding: 8px 6px;
        border: none;
        border-bottom: 2px solid #c0c0c0;
        font-weight: 600;
    }
    QHeaderView::section:horizontal { border-right: 1px solid #c0c0c0; }

    QStatusBar { background-color: #f2f2f2; color: #555555; }
    QStatusBar::item { border: none; }
    QDialog { background-color: #ffffff; }
    QTextEdit {
        background-color: #ffffff;
        border: 1px solid #c0c0c0;
        border-radius: 6px;
        color: #1a1a1a;
        font-size: 12px;
    }
    QCheckBox { color: #1a1a1a; }
    QMessageBox { background-color: #ffffff; }
)";


UninstallerWindow::UninstallerWindow(QWidget* parent) : QMainWindow(parent) {}

void UninstallerWindow::updateFindList() {
    findlist.clear();
    findlist.push_back(0); // Normal
    findlist.push_back(1); // WindowsInstaller
    if (m_showSystemComponents) {
        findlist.push_back(2); // SystemComponent
    }
}

SoftwareInfo* UninstallerWindow::softwareAtRow(int row) const {
    if (row < 0 || row >= m_tableWidget->rowCount()) return nullptr;
    QTableWidgetItem* item = m_tableWidget->item(row, 0);
    if (!item) return nullptr;
    return reinterpret_cast<SoftwareInfo*>(item->data(Qt::UserRole).value<qintptr>());
}

void UninstallerWindow::run() {
    // 设置窗口标题栏与任务栏图标（exe 内嵌的 IDI_APP_ICON）
    setWindowIcon(QApplication::windowIcon());
    loadLanguageSetting();   // 启动前恢复上次选择的语言，setupUI 才会用正确的语言建界面
    // ⑨ 恢复持久化的主题
    {
        QSettings st("Uninstaller", "uninstaller");
        m_theme = st.value("theme", 0).toInt();
        setTheme(m_theme);
    }
    setupUI();
    built_list(/*allowCache=*/true);   // 启动：命中1小时内缓存则跳过扫描与进度条
    loadSoftwareList();
    show();
    showUpdatePopup();   // 启动即弹出更新日志
}

void UninstallerWindow::fresh() {
    setupUI();
    show();
}

void UninstallerWindow::built_list(bool allowCache) {
    // 启动时使用缓存：若1小时内有缓存则直接读取，跳过注册表扫描与进度条
    if (allowCache && loadSoftwareCache()) {
        return;
    }

    m_softwareList = Registry::getAllInstalledSoftware();
    len = m_softwareList.size();
    total_size = 0;
    if (len == 0) {
        saveSoftwareCache();
        return;
    }
    QProgressDialog progress(
        getlang(0x2u).toString(),
        getlang(0x3u).toString(),
        0,
        len,
        this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(1000);

    // 并行初始化（注册表读取 + 体积扫描 + 进程检测），多核加速。
    // registryInit 仅调用文件/进程/注册表等系统 API，不触碰任何 GUI/窗口子系统，
    // 因此在后台线程并发执行是安全的。期间用 QEventLoop 泵事件保持 UI 响应，
    // 进度条随 QtConcurrent 报告实时推进（替代原先逐元素串行 + 手动泵事件）。
    QFutureWatcher<void> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcher<void>::finished, &loop, &QEventLoop::quit);
    connect(&watcher, &QFutureWatcher<void>::progressValueChanged,
            &progress, [&](int v) {
                progress.setValue(v);
                QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            });
    watcher.setFuture(QtConcurrent::map(m_softwareList,
        [](SoftwareInfo& sw) { sw.registryInit(); }));
    loop.exec();

    for (auto& sw : m_softwareList) total_size += sw.size.size;
    progress.close();

    // 实时扫描完成后写缓存（供1小时内再次启动时免加载条）
    saveSoftwareCache();
}

bool UninstallerWindow::loadSoftwareCache() {
    const QString path = QCoreApplication::applicationDirPath() + QStringLiteral("/uninstaller_cache.json");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (doc.isNull() || !doc.isObject()) return false;
    QJsonObject root = doc.object();
    const qint64 ts = root.value(QStringLiteral("timestamp")).toVariant().toLongLong();
    // 仅在1小时内有效（3600 * 1000 ms）
    if (QDateTime::currentMSecsSinceEpoch() - ts > 3600LL * 1000LL) return false;
    const QJsonArray items = root.value(QStringLiteral("items")).toArray();
    if (items.isEmpty()) return false;

    m_softwareList.clear();
    m_softwareList.reserve(items.size());
    for (const QJsonValue& v : items) {
        const QJsonObject o = v.toObject();
        SoftwareInfo sw;
        sw.displayName     = o.value(QStringLiteral("displayName")).toString().toStdString();
        sw.displayVersion  = o.value(QStringLiteral("displayVersion")).toString().toStdString();
        sw.installDate     = o.value(QStringLiteral("installDate")).toString().toStdString();
        sw.installLocation = o.value(QStringLiteral("installLocation")).toString().toStdString();
        sw.uninstallString = o.value(QStringLiteral("uninstallString")).toString().toStdString();
        sw.publisher       = o.value(QStringLiteral("publisher")).toString().toStdString();
        sw.displayIcon     = o.value(QStringLiteral("displayIcon")).toString().toStdString();
        sw.helpLink        = o.value(QStringLiteral("helpLink")).toString().toStdString();
        sw.urlInfoAbout    = o.value(QStringLiteral("urlInfoAbout")).toString().toStdString();
        sw.regPath         = o.value(QStringLiteral("regPath")).toString().toStdString();
        sw.orgPath         = o.value(QStringLiteral("orgPath")).toString().toStdString();
        sw.size.size       = o.value(QStringLiteral("size")).toVariant().toDouble();
        sw.size.format_size = o.value(QStringLiteral("format_size")).toString().toStdString();
        sw.hive            = reinterpret_cast<HKEY>(o.value(QStringLiteral("hive")).toVariant().toLongLong());
        sw.isRunningTime     = o.value(QStringLiteral("isRunningTime")).toBool();
        sw.isSystemComponent = o.value(QStringLiteral("isSystemComponent")).toBool();
        sw.isWindowsInstaller = o.value(QStringLiteral("isWindowsInstaller")).toBool();
        sw.isOrphaned        = o.value(QStringLiteral("isOrphaned")).toBool();
        m_softwareList.push_back(sw);
    }
    len = static_cast<int>(m_softwareList.size());
    total_size = filesize_t();
    for (auto& s : m_softwareList) total_size += s.size.size;
    return true;
}

void UninstallerWindow::saveSoftwareCache() {
    QJsonArray items;
    for (const auto& s : m_softwareList) {
        QJsonObject o;
        o[QStringLiteral("displayName")]     = QString::fromStdString(s.displayName);
        o[QStringLiteral("displayVersion")]  = QString::fromStdString(s.displayVersion);
        o[QStringLiteral("installDate")]     = QString::fromStdString(s.installDate);
        o[QStringLiteral("installLocation")] = QString::fromStdString(s.installLocation);
        o[QStringLiteral("uninstallString")] = QString::fromStdString(s.uninstallString);
        o[QStringLiteral("publisher")]       = QString::fromStdString(s.publisher);
        o[QStringLiteral("displayIcon")]     = QString::fromStdString(s.displayIcon);
        o[QStringLiteral("helpLink")]        = QString::fromStdString(s.helpLink);
        o[QStringLiteral("urlInfoAbout")]    = QString::fromStdString(s.urlInfoAbout);
        o[QStringLiteral("regPath")]         = QString::fromStdString(s.regPath);
        o[QStringLiteral("orgPath")]         = QString::fromStdString(s.orgPath);
        o[QStringLiteral("size")]            = static_cast<double>(s.size.size);
        o[QStringLiteral("format_size")]     = QString::fromStdString(s.size.format_size);
        o[QStringLiteral("hive")]            = static_cast<qint64>(reinterpret_cast<quintptr>(s.hive));
        o[QStringLiteral("isRunningTime")]     = s.isRunningTime;
        o[QStringLiteral("isSystemComponent")] = s.isSystemComponent;
        o[QStringLiteral("isWindowsInstaller")] = s.isWindowsInstaller;
        o[QStringLiteral("isOrphaned")]        = s.isOrphaned;
        items.append(o);
    }
    QJsonObject root;
    root[QStringLiteral("timestamp")] = QDateTime::currentMSecsSinceEpoch();
    root[QStringLiteral("items")] = items;
    QJsonDocument doc(root);
    const QString path = QCoreApplication::applicationDirPath() + QStringLiteral("/uninstaller_cache.json");
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(doc.toJson(QJsonDocument::Compact));
    }
}

void UninstallerWindow::closeEvent(QCloseEvent* event) {
    // 关闭前台窗口即结束整个程序（含后台），不缩到系统托盘
    event->accept();

    // 先立即隐藏窗口：即使后台线程还没清理完，用户也看不到“未响应”状态。
    hide();
    QApplication::processEvents();

    // 请求后台图标线程退出；若线程卡在 ExtractIconExW/网络盘/C盘锁上，
    // 仅调用 QCoreApplication::quit() Qt 会等待线程结束，导致进程残留。
    if (m_iconThread && m_iconThread->isRunning()) {
        m_iconThread->requestInterruption();
        m_iconThread->quit();
        if (!m_iconThread->wait(1000)) {
            // 强制终止（图标提取函数本身阻塞时只能如此）
            m_iconThread->terminate();
            m_iconThread->wait(200);
        }
        m_iconThread = nullptr;
    }

    // 立即结束整个进程：即使后台线程/WinAPI 仍阻塞，也不给任何残留机会。
    // 当前程序关闭时无需写回数据（缓存已实时写入），直接 ExitProcess 是安全的。
    ::ExitProcess(0);
}


void UninstallerWindow::sorting() {
    m_swlist.clear();
    int len = m_softwareList.size();
    for (int i = 0; i < len; i++) {
        auto sw = &m_softwareList[i];
        if (sw->isRunningTime)m_swlist[3].push_back(sw);
        else if (sw->isSystemComponent)m_swlist[2].push_back(sw);
        else if (sw->isWindowsInstaller)m_swlist[1].push_back(sw);
        else if (sw->displayName.empty())m_swlist[4].push_back(sw);
        else m_swlist[0].push_back(sw);
    }

}

// ================= 后台图标懒加载 =================
// 进度条阶段只放默认图标，避免任何文件访问；列表显示后由本工作线程在后台逐个提取真实图标，
// 提取完通过信号回到 UI 线程 setIcon。这样即使 C 盘被锁定 / 网络盘慢 / 安装目录巨大，
// 主界面早已显示出来，不会因进度条卡死而无响应。
class IconLoaderWorker : public QObject {
    Q_OBJECT
public:
    explicit IconLoaderWorker(QObject* parent = nullptr) : QObject(parent) {}
    // (sw 指针, 软件名) 列表，后台线程据此提取图标。
    QVector<QPair<qlonglong, SoftwareInfo*>> items;

public slots:
    void run() {
        for (const auto& kv : std::as_const(items)) {
            if (QThread::currentThread()->isInterruptionRequested()) break;
            SoftwareInfo* sw = kv.second;
            if (!sw) continue;
            // 后台不启用“运行进程扫描”（EnumWindows 在后台线程有跨线程死锁风险），
            // 未命中的行会在 onIconReady 里用 GUI 线程完整版兜底。
            QImage img = softwareIconImage(sw, /*allowProcessScan=*/false);
            emit iconReady(kv.first, img);
        }
        emit finished();
    }

signals:
    void iconReady(qlonglong swPtr, const QImage& img);
    void finished();
};

void UninstallerWindow::startBackgroundIconLoad() {
    // 注册 QImage 元类型（跨线程信号传递 QImage 需要）。
    static bool once = (qRegisterMetaType<QImage>(), true);

    QVector<QPair<qlonglong, SoftwareInfo*>> items;
    int rows = m_tableWidget->rowCount();
    for (int r = 0; r < rows; ++r) {
        QTableWidgetItem* item = m_tableWidget->item(r, 0);
        if (!item) continue;
        qintptr p = item->data(Qt::UserRole).value<qintptr>();
        if (p) items.append({static_cast<qlonglong>(p), reinterpret_cast<SoftwareInfo*>(p)});
    }
    if (items.isEmpty()) return;

    // 若已有后台图标线程在跑，先请求中断并等待退出，避免旧线程与新线程并发访问图标句柄/GDI。
    if (m_iconThread) {
        if (m_iconThread->isRunning()) {
            m_iconThread->requestInterruption();
            m_iconThread->quit();
            m_iconThread->wait(1000);
        }
        m_iconThread->deleteLater();
        m_iconThread = nullptr;
    }

    auto* worker = new IconLoaderWorker;
    worker->items = items;
    auto* thread = new QThread(this);
    m_iconThread = thread;
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &IconLoaderWorker::run);
    connect(worker, &IconLoaderWorker::iconReady,
            this, &UninstallerWindow::onIconReady, Qt::QueuedConnection);
    connect(worker, &IconLoaderWorker::finished, thread, &QThread::quit);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, [this]() {
        // thread 自己 deleteLater 后把成员指针清空（不 double-delete）
        if (m_iconThread && m_iconThread->isFinished()) m_iconThread = nullptr;
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void UninstallerWindow::onIconReady(qlonglong swPtr, const QImage& img) {
    // 按指针找到当前行（用户可能在后台加载期间排序/过滤，行号会变，故用指针匹配，稳健）。
    int rows = m_tableWidget->rowCount();
    for (int r = 0; r < rows; ++r) {
        QTableWidgetItem* item = m_tableWidget->item(r, 0);
        if (!item) continue;
        if (item->data(Qt::UserRole).value<qintptr>() != static_cast<qintptr>(swPtr)) continue;
        if (!img.isNull()) {
            QPixmap pm = QPixmap::fromImage(img).scaled(20, 20, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            item->setIcon(QIcon(pm));
        } else {
            // 后台只做了文件提取（不含进程扫描），未命中用 GUI 线程完整版兜底
            // （含进程扫描 + 默认图标），保证最终图标正确。
            SoftwareInfo* sw = reinterpret_cast<SoftwareInfo*>(static_cast<qintptr>(swPtr));
            item->setIcon(iconForSoftware(sw));
        }
        break;
    }
}

void UninstallerWindow::loadSoftwareList() {
    updateFindList();
    sorting();
    filesize_t ts = 0;
    int i{ 0 }, n{ 0 };
    QProgressDialog progress(
        getlang(0x6u).toString(),
        getlang(0x3u).toString(),
        0,
        len,
        this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(100);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    m_tableWidget->setSortingEnabled(false);
    m_tableWidget->setRowCount(len);
    for (const auto j : m_swlist) {
        if (progress.wasCanceled()) break;
        if (!func::similarly(j.first, this->findlist))continue;
        for (const auto sw : j.second) {
            auto* nameItem = new QTableWidgetItem(QString::fromStdString(sw->displayName));
            nameItem->setData(Qt::UserRole, QVariant::fromValue(reinterpret_cast<qintptr>(sw)));
            // 列表填充时使用“快速图标”：只从 DisplayIcon / UninstallString 指向的单个文件提取，
            // 不扫描安装目录/不枚举进程。这样进度条不会卡死，同时能恢复大部分真实图标。
            nameItem->setIcon(iconForSoftwareFast(sw));
            m_tableWidget->setItem(i, 0, nameItem);

            m_tableWidget->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(sw->displayVersion)));
            m_tableWidget->setItem(i, 2, new QTableWidgetItem(QString::fromStdString(sw->installDate)));

            auto* sizeItem = new SizeTableItem(
                QString::fromStdString(sw->size.get()),
                static_cast<qint64>(sw->size.size));
            m_tableWidget->setItem(i, 3, sizeItem);

            m_tableWidget->setItem(i, 4, new QTableWidgetItem(QString::fromStdString(sw->publisher)));
            m_tableWidget->setItem(i, 5, new QTableWidgetItem(QString::fromStdString(sw->installLocation)));

            // 状态列：残留项标记红色“残留”
            if (sw->isOrphaned) {
                auto* statusItem = new QTableWidgetItem(QString::fromUtf8(u8"残留"));
                QFont sf = statusItem->font();
                sf.setBold(true);
                statusItem->setFont(sf);
                statusItem->setForeground(QColor(0xE0, 0x5A, 0x5A));
                m_tableWidget->setItem(i, 6, statusItem);
                // 名称也染成警告色，方便一眼识别
                QFont nf = nameItem->font();
                nameItem->setForeground(QColor(0xD9, 0x7A, 0x7A));
                nameItem->setFont(nf);
            } else if (isCriticalSystemItem(sw)) {
                // ⑦ 系统关键项：灰显并标注“不建议卸载”
                auto* statusItem = new QTableWidgetItem(QString::fromUtf8(u8"系统关键"));
                statusItem->setForeground(QColor(0x9E, 0x9E, 0x9E));
                m_tableWidget->setItem(i, 6, statusItem);
                nameItem->setForeground(QColor(0x9E, 0x9E, 0x9E));
            } else {
                m_tableWidget->setItem(i, 6, new QTableWidgetItem(QString::fromUtf8(u8"正常")));
            }

            ts += sw->size.size;
            i++, n++;
            progress.setValue(i);
            progress.setLabelText(QString("%1 (%2/%3)")
                .arg(QString::fromStdString(sw->displayName))
                .arg(n)
                .arg(len));
            if ((n & 0x1F) == 0) {
                QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            }
            if (progress.wasCanceled()) break;
        }
    }
    m_tableWidget->setRowCount(i);
    m_tableWidget->resizeColumnsToContents();
    m_tableWidget->setSortingEnabled(true);
    m_tableWidget->sortItems(0, Qt::AscendingOrder);
    progress.close();

    // 后台图标懒加载已禁用：ExtractIconExW/DrawIconEx/GDI 在后台线程与 Qt 主线程
    // 共享 GUI 子系统，可能触发竞争导致主窗口“未响应”。先临时禁用，后续改为
    // 仅提取可见区域图标（主线程按需执行）再恢复。
    // startBackgroundIconLoad();

    // 状态栏统一由 filterSoftware 更新为可见行数/可见大小；
    // 若搜索框为空则显示全部，非空则保持过滤状态。
    filterSoftware();
}

bool UninstallerWindow::tick(const ll& row) {
    if (!softwareAtRow(row)) {
        QMessageBox::warning(this, getlang(0x8u).toString(), getlang(0x9u).toString());
        return 1;
    }
    return 0;
}

// 执行单个卸载（不含确认/预览），单条与批量共用。返回是否成功。
bool UninstallerWindow::doUninstall(SoftwareInfo* software) {
    if (!software) return false;
    QString name = QString::fromStdString(software->displayName);
    QProgressDialog progress(getlang(0xCu).toString().arg(name),
        getlang(0x3u).toString(), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.show();
    QApplication::processEvents();
    bool success = Registry::uninstallSoftware(*software);
    progress.close();
    return success;
}

void UninstallerWindow::uninstallSelected() {
    int row = m_tableWidget->currentRow();
    if (tick(row))return ;

    auto software = softwareAtRow(row);
    if (!software) return;

    // ⑦ 系统关键项拦截：Windows 更新 / 驱动 / 系统组件 等不允许卸载。
    if (isCriticalSystemItem(software)) {
        QMessageBox::critical(this, QString::fromUtf8(u8"无法卸载"),
            QString::fromUtf8(u8"「%1」被识别为系统关键项（Windows 更新 / 驱动 / 系统组件）。\n卸载它可能导致系统不稳定，已阻止该操作。").arg(QString::fromStdString(software->displayName)));
        return;
    }

    QString name = QString::fromStdString(software->displayName);
    QString ver = QString::fromStdString(software->displayVersion);
    QString cmd = QString::fromStdString(Registry::getUninstallCommand(*software));

    // ④ 卸载前预览：明确告知将要卸载的程序、版本、体积与卸载命令，确认后再执行。
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(getlang(0xAu).toString());
    box.setText(getlang(0xBu).toString().arg(name).arg(ver));
    QString info = getlang(0x38).toString();
    if (!cmd.isEmpty()) info += "\n" + stripCommandQuotes(cmd);
    info += QString::fromUtf8(u8"\n\n预计释放空间：%1").arg(QString::fromStdString(software->size.get()));
    if (software->isOrphaned) info += QString::fromUtf8(u8"\n注意：该程序已被判定为“残留项”（卸载程序不存在），卸载操作可能仅清理注册表项。");
    box.setInformativeText(info);
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::No);
    if (box.exec() != QMessageBox::Yes) return;

    if (m_busy) return;
    m_busy = true;

    bool success = doUninstall(software);

    if (success) {
        QMessageBox::StandardButton result = QMessageBox::question(
            this, getlang(0xDu).toString(), getlang(0xEu).toString(),
            QMessageBox::Yes | QMessageBox::No);

        if (result == QMessageBox::Yes) {
            scanResiduals();
        }
        // ② 重新扫描注册表，让列表反映真实状态（已卸载的条目会消失）
        built_list();
        loadSoftwareList();
    }
    else {
        QMessageBox::critical(this, getlang(0xFu).toString(), getlang(0x10u).toString());
    }

    m_busy = false;
}

// ③ 批量卸载：对当前所有选中行逐个卸载（跳过系统关键项），结束统一重扫。
void UninstallerWindow::batchUninstall() {
    QList<SoftwareInfo*> selected;
    QList<int> rows;
    QItemSelectionModel* sel = m_tableWidget->selectionModel();
    if (!sel) return;
    for (const QModelIndex& idx : sel->selectedRows()) {
        int r = idx.row();
        auto sw = softwareAtRow(r);
        if (sw) { rows.append(r); selected.append(sw); }
    }
    if (selected.isEmpty()) {
        QMessageBox::information(this, QString::fromUtf8(u8"批量卸载"), QString::fromUtf8(u8"请先按住 Ctrl / Shift 在列表中选择要卸载的软件。"));
        return;
    }

    // ⑦ 过滤掉系统关键项并提示
    QStringList blocked;
    QList<SoftwareInfo*> toUninstall;
    filesize_t totalSize;
    for (auto sw : selected) {
        if (isCriticalSystemItem(sw)) blocked.append(QString::fromStdString(sw->displayName));
        else { toUninstall.append(sw); totalSize += sw->size.size; }
    }
    if (toUninstall.isEmpty()) {
        QMessageBox::critical(this, QString::fromUtf8(u8"无法卸载"), QString::fromUtf8(u8"所选条目均为系统关键项，已阻止批量卸载。"));
        return;
    }

    // ④ 批量预览：列出将卸载的程序与合计释放空间
    QString preview = QString::fromUtf8(u8"即将批量卸载 %1 个程序，预计释放空间：%2\n\n").arg(toUninstall.size()).arg(QString::fromStdString(totalSize.get()));
    for (auto sw : toUninstall) {
        preview += "• " + QString::fromStdString(sw->displayName) + "\n";
    }
    if (!blocked.isEmpty()) {
        preview += QString::fromUtf8(u8"\n已跳过 %1 个系统关键项：\n").arg(blocked.size());
        for (const QString& b : blocked) preview += "• " + b + "\n";
    }
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QString::fromUtf8(u8"批量卸载预览"));
    box.setText(QString::fromUtf8(u8"确认批量卸载以下程序？此操作不可撤销。"));
    box.setDetailedText(preview);
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::No);
    if (box.exec() != QMessageBox::Yes) return;

    if (m_busy) return;
    m_busy = true;
    int ok = 0;
    for (auto sw : toUninstall) {
        if (doUninstall(sw)) ++ok;
    }
    m_busy = false;

    QMessageBox::information(this, QString::fromUtf8(u8"批量卸载完成"),
        QString::fromUtf8(u8"成功卸载 %1 / %2 个程序。").arg(ok).arg(toUninstall.size()));

    // ② 统一重扫
    built_list();
    loadSoftwareList();
}

// ⑧ 在注册表中定位：写入 LastKey 再启动 regedit，使其自动跳转到该卸载项。
void UninstallerWindow::locateInRegistry() {
    int row = m_tableWidget->currentRow();
    if (tick(row)) return;
    auto sw = softwareAtRow(row);
    if (!sw) return;

    QString hiveName = (sw->hive == HKEY_LOCAL_MACHINE) ? "HKEY_LOCAL_MACHINE" : "HKEY_CURRENT_USER";
    QString key = hiveName + "\\" + QString::fromStdString(sw->regPath);

    // 若 regedit 已打开，先关闭以使其重新读取 LastKey。
    QProcess::execute("taskkill", QStringList() << "/IM" << "regedit.exe" << "/F");
    QSettings lastKey("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Applets\\Regedit",
                      QSettings::NativeFormat);
    lastKey.setValue("LastKey", key);
    QProcess::startDetached("regedit.exe");
}

// ⑨ 切换亮/暗主题并持久化。
void UninstallerWindow::setTheme(int t) {
    m_theme = t;
    QSettings s("Uninstaller", "uninstaller");
    s.setValue("theme", t);

    QString sheet;
    if (t == 1) {
        sheet = QString::fromUtf8(kAppStyleSheet)
            + QString::fromUtf8(
                "QProgressDialog { background-color:#181a1e; color:#e0e3e8; }"
                "QProgressBar { background-color:#202328; color:#e0e3e8; border:1px solid #3c424d; border-radius:4px; text-align:center; }"
                "QProgressBar::chunk { background-color:#4a90e2; border-radius:2px; }");
    } else {
        sheet = QString::fromUtf8(kLightStyleSheet)
            + QString::fromUtf8(
                "QProgressDialog { background-color:#ffffff; color:#1a1a1a; }"
                "QProgressBar { background-color:#e8e8e8; color:#1a1a1a; border:1px solid #c0c0c0; border-radius:4px; text-align:center; }"
                "QProgressBar::chunk { background-color:#4a90e2; border-radius:2px; }");
    }
    qApp->setStyleSheet(sheet);
    // 强制刷新所有已打开窗口的样式
    for (QWidget* w : QApplication::allWidgets()) {
        if (w) { w->style()->unpolish(w); w->style()->polish(w); }
    }

    // 同步视图菜单的勾选状态（互斥单选，唯一选中当前主题）
    if (m_lightThemeAct) m_lightThemeAct->setChecked(t == 0);
    if (m_darkThemeAct) m_darkThemeAct->setChecked(t == 1);
}

// ③ 批量删除选中项的磁盘残留（送回收站）。
void UninstallerWindow::batchDeleteResiduals() {
    QItemSelectionModel* sel = m_tableWidget->selectionModel();
    if (!sel) return;
    QList<SoftwareInfo*> selected;
    for (const QModelIndex& idx : sel->selectedRows()) {
        auto sw = softwareAtRow(idx.row());
        if (sw) selected.append(sw);
    }
    if (selected.isEmpty()) {
        QMessageBox::information(this, QString::fromUtf8(u8"批量删除残留"),
            QString::fromUtf8(u8"请先按住 Ctrl / Shift 在列表中选择要清理残留的软件。"));
        return;
    }

    // 先汇总所有残留
    std::vector<std::string> allResiduals;
    for (auto sw : selected) {
        auto r = Registry::scanResidualFiles(*sw, sw->isOrphaned);
        for (const auto& f : r) allResiduals.push_back(f);
    }
    if (allResiduals.empty()) {
        QMessageBox::information(this, QString::fromUtf8(u8"批量删除残留"),
            QString::fromUtf8(u8"所选软件的磁盘残留均已清理，没有发现新的残留文件。"));
        return;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QString::fromUtf8(u8"批量删除残留预览"));
    box.setText(QString::fromUtf8(u8"即将删除 %1 个残留文件/目录（送回收站）。确认吗？").arg(allResiduals.size()));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::No);
    if (box.exec() != QMessageBox::Yes) return;

    if (m_busy) return;
    m_busy = true;
    bool ok = Registry::deleteResidualFiles(allResiduals);
    m_busy = false;

    if (ok) {
        QMessageBox::information(this, QString::fromUtf8(u8"已完成"),
            QString::fromUtf8(u8"已删除 %1 个残留文件/目录（已送回收站，可恢复）。").arg(allResiduals.size()));
        // ② 重扫
        built_list();
        loadSoftwareList();
    } else {
        QString hint = informat::diagnoseDeleteFailure(allResiduals);
        QMessageBox::warning(this, getlang(0xFu).toString(),
            getlang(0x1A).toString() + "\n\n" + hint);
    }
}

bool UninstallerWindow::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_tableWidget->viewport() && event->type() == QEvent::ContextMenu) {
        auto* ctx = static_cast<QContextMenuEvent*>(event);
        onTableContextMenu(ctx->pos());
        return true; // 已处理，不再走默认菜单
    }
    return QMainWindow::eventFilter(obj, event);
}

void UninstallerWindow::onTableContextMenu(const QPoint& pos) {
    QTableWidgetItem* item = m_tableWidget->itemAt(pos);
    if (!item) return;
    int row = item->row();
    if (!softwareAtRow(row)) return;

    QMenu menu(this);
    QAction* actUninstall = menu.addAction(getlang(0x1f).toString());
    QAction* actScan = menu.addAction(getlang(0x20).toString());
    QAction* actDetail = menu.addAction(getlang(0x21).toString());
    QAction* actOpen = menu.addAction(getlang(0x39).toString());
    menu.addSeparator();
    QAction* actCopy = menu.addAction(getlang(0x2F).toString());
    QAction* actLocate = menu.addAction(QString::fromUtf8(u8"在注册表中定位"));
    QAction* actDelReg = menu.addAction(QString::fromUtf8(u8"删除残留注册表项"));
    QAction* actForceDel = menu.addAction(QString::fromUtf8(u8"强制删除此条目"));

    // 选中右键所在行，再触发与按钮相同的逻辑
    connect(actUninstall, &QAction::triggered, this, [this, row]() {
        m_tableWidget->setCurrentCell(row, 0);
        uninstallSelected();
    });
    connect(actScan, &QAction::triggered, this, [this, row]() {
        m_tableWidget->setCurrentCell(row, 0);
        scanResiduals();
    });
    connect(actDetail, &QAction::triggered, this, [this, row]() {
        m_tableWidget->setCurrentCell(row, 0);
        showDetails();
    });
    connect(actOpen, &QAction::triggered, this, [this, row]() {
        m_tableWidget->setCurrentCell(row, 0);
        openFileLocation();
    });
    connect(actLocate, &QAction::triggered, this, [this, row]() {
        m_tableWidget->setCurrentCell(row, 0);
        locateInRegistry();
    });
    connect(actCopy, &QAction::triggered, this, [this, row]() {
        m_tableWidget->setCurrentCell(row, 0);
        copyUninstallCommand();
    });
    connect(actDelReg, &QAction::triggered, this, [this, row]() {
        m_tableWidget->setCurrentCell(row, 0);
        deleteRegistryEntry();
    });
    connect(actForceDel, &QAction::triggered, this, [this, row]() {
        m_tableWidget->setCurrentCell(row, 0);
        forceDeleteEntry();
    });

    menu.exec(m_tableWidget->viewport()->mapToGlobal(pos));
}

void UninstallerWindow::scanResiduals() {
    int row = m_tableWidget->currentRow();
    if (tick(row))return;

    auto software = softwareAtRow(row);

    QProgressDialog progress(getlang(0x11u).toString(), getlang(0x3).toString(), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.show();
    QApplication::processEvents();

    auto residuals = Registry::scanResidualFiles(*software);
    progress.close();

    if (residuals.empty()) {
        QMessageBox::information(this, getlang(0x12).toString(), getlang(0x13).toString());
        return;
    }

    // 显示残留文件列表
    QDialog dialog(this);
    dialog.setWindowTitle(getlang(0x14).toString());
    dialog.resize(600, 400);

    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QTextEdit* textEdit = new QTextEdit(&dialog);
    textEdit->setReadOnly(true);

    QString content;
    content += getlang(0x16).toString().arg(QString::fromStdString(software->displayName));
    content += getlang(0x17).toString().arg(residuals.size());

    for (const auto& file : residuals) {
        content += QString::fromStdString(file) + "\n";
    }

    textEdit->setText(content);
    layout->addWidget(textEdit);

    QPushButton* deleteBtn = new QPushButton(getlang(0x18).toString(), &dialog);
    QPushButton* cancelBtn = new QPushButton(getlang(0x3).toString(), &dialog);

    QHBoxLayout* btnLayout = new QHBoxLayout();
    btnLayout->addWidget(deleteBtn);
    btnLayout->addWidget(cancelBtn);
    layout->addLayout(btnLayout);

    connect(deleteBtn, &QPushButton::clicked, [&]() {
        QMessageBox::StandardButton confirm = QMessageBox::question(
            &dialog,
            QString::fromUtf8(u8"确认删除残留文件"),
            QString::fromUtf8(u8"即将删除上面列出的 %1 个残留文件/目录，此操作不可撤销。\n确定继续吗？").arg(residuals.size()),
            QMessageBox::Yes | QMessageBox::No);
        if (confirm != QMessageBox::Yes) return;
        if (Registry::deleteResidualFiles(residuals)) {
            QMessageBox::information(&dialog, getlang(0xD).toString(), getlang(0x19).toString());
            dialog.accept();
            // ② 删除成功后重新扫描，让列表反映真实状态
            built_list();
            loadSoftwareList();
        }
        else {
            // ④ C 盘被锁定/无权限/文件占用时，给出可操作的友好提示，而非笼统报错。
            QString hint = informat::diagnoseDeleteFailure(residuals);
            QMessageBox::warning(&dialog, getlang(0xF).toString(),
                                 getlang(0x1A).toString() + "\n\n" + hint);
        }
        });

    connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);

    dialog.exec();
}

void UninstallerWindow::deleteRegistryEntry() {
    int row = m_tableWidget->currentRow();
    if (tick(row)) return;

    auto sw = softwareAtRow(row);
    if (!sw) return;

    QString name = QString::fromStdString(sw->displayName);
    QString regPath = QString::fromStdString(sw->orgPath);

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QString::fromUtf8(u8"删除残留注册表项"));
    box.setText(QString::fromUtf8(u8"确定要从注册表删除该条目吗？\n\n软件: %1\n注册表位置: %2").arg(name).arg(regPath));
    box.setInformativeText(QString::fromUtf8(u8"此操作会直接移除该软件的注册表卸载项（适用于已卸载/残留的软件）。删除后无法撤销，且不会删除任何磁盘文件。"));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::No);
    if (box.exec() != QMessageBox::Yes) return;

    if (m_busy) return;
    m_busy = true;
    bool ok = Registry::deleteRegistryKey(sw->hive, sw->regPath);
    m_busy = false;

    if (ok) {
        QMessageBox::information(this, QString::fromUtf8(u8"成功"), QString::fromUtf8(u8"注册表项已删除。"));
        // 重新扫描注册表，让列表反映真实状态（残留条目会消失）
        built_list();
        loadSoftwareList();
    }
    else {
        QMessageBox::critical(this, QString::fromUtf8(u8"失败"),
            QString::fromUtf8(u8"删除注册表项失败。\n若为 HKLM 项，可能被安全软件拦截或需要管理员权限；若为 HKCU 项，则该项可能已被删除。"));
    }
}

// 强制删除此条目：绕过残留自动检测，直接移除注册表卸载项，并强制扫描/删除其磁盘残留。
// 适用于“程序打不开/想强制当残留删”的场景。操作不可撤销，故二次确认 + 明确风险提示。
void UninstallerWindow::forceDeleteEntry() {
    int row = m_tableWidget->currentRow();
    if (tick(row)) return;

    auto sw = softwareAtRow(row);
    if (!sw) return;

    QString name = QString::fromStdString(sw->displayName);
    QString regPath = QString::fromStdString(sw->orgPath);

    QMessageBox box(this);
    box.setIcon(QMessageBox::Critical);
    box.setWindowTitle(QString::fromUtf8(u8"强制删除此条目"));
    box.setText(QString::fromUtf8(u8"⚠️ 即将强制删除该软件条目：\n\n软件: %1\n注册表位置: %2").arg(name).arg(regPath));
    box.setInformativeText(QString::fromUtf8(u8"此操作会：\n"
        "1) 直接从注册表移除该软件的卸载项；\n"
        "2) 强制扫描并删除其残留/安装目录（含 AppData、ProgramData、开始菜单下以软件名命名的目录）。\n\n"
        "该软件当前未被判定为“残留”（文件可能仍在使用中），强制删除可能误删正在使用的软件数据，且操作不可撤销。确定继续吗？"));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::No);
    if (box.exec() != QMessageBox::Yes) return;

    if (m_busy) return;
    m_busy = true;

    // 1) 删除注册表项
    bool okReg = Registry::deleteRegistryKey(sw->hive, sw->regPath);

    // 2) 强制扫描并删除磁盘残留（绕过 isOrphaned 判断）
    auto residuals = Registry::scanResidualFiles(*sw, /*force=*/true);
    bool okFiles = true;
    if (!residuals.empty()) {
        okFiles = Registry::deleteResidualFiles(residuals);
    }

    m_busy = false;

    if (okReg) {
        QString extra;
        if (!residuals.empty()) {
            extra = QString::fromUtf8(u8"\n已一并删除 %1 个磁盘残留/目录。").arg(residuals.size());
        }
        else {
            extra = QString::fromUtf8(u8"\n未发现可删除的磁盘残留目录。");
        }
        if (!okFiles) {
            // ④ C 盘被锁定/无权限时给出可操作的友好提示。
            QString hint = informat::diagnoseDeleteFailure(residuals);
            extra += QString::fromUtf8(u8"\n（部分残留文件删除失败：%1）").arg(hint);
        }
        QMessageBox::information(this, QString::fromUtf8(u8"已完成"),
            QString::fromUtf8(u8"已强制删除该软件条目。%1").arg(extra));
        // 重新扫描注册表，让列表反映真实状态（该条目会消失）
        built_list();
        loadSoftwareList();
    }
    else {
        QMessageBox::critical(this, QString::fromUtf8(u8"失败"),
            QString::fromUtf8(u8"删除注册表项失败。\n若为 HKLM 项，可能被安全软件拦截或需要管理员权限；若为 HKCU 项，则该项可能已被删除。"));
    }
}

// 卸载本程序：从自身安装目录找到随附的 uninst.exe（纯 Win32 卸载桩），
// 启动它之后本程序立即退出，由 uninst.exe 负责删除安装目录、快捷方式与注册表项，
// 从而规避“删正在运行的自己”的自锁（uninst.exe 不加载 Qt DLL，且删除动作在它退出后才发生）。
void UninstallerWindow::uninstallSelf() {
    // 1) 优先使用与本程序同目录的 uninst.exe（安装版与便携版均随附）。
    QString appDir = QCoreApplication::applicationDirPath();
    QString uninstPath = QDir::toNativeSeparators(appDir + "/uninst.exe");

    // 2) 若同目录没有，则从注册表 Uninstall 项读取 UninstallString 定位。
    if (!QFile::exists(uninstPath)) {
        HKEY hk;
        const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\UninstallerManager";
        if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
            wchar_t buf[1024] = {}; DWORD sz = sizeof(buf);
            if (RegQueryValueExW(hk, L"UninstallString", 0, nullptr, (BYTE*)buf, &sz) == ERROR_SUCCESS) {
                QString ustr = QString::fromWCharArray(buf).trimmed();
                ustr = stripQuotes(ustr).trimmed();
                if (!ustr.isEmpty() && QFile::exists(ustr)) {
                    uninstPath = QDir::toNativeSeparators(ustr);
                }
            }
            RegCloseKey(hk);
        }
    }

    // 3) 两种途径都没有 uninst.exe：说明是未打包的调试/便携运行，提示用户手动删除目录。
    if (!QFile::exists(uninstPath)) {
        QMessageBox::information(this, QString::fromUtf8(u8"卸载本程序"),
            QString::fromUtf8(u8"未找到卸载程序（uninst.exe）。\n\n"
                "如果你是从源码目录或便携包直接运行，直接删除整个程序文件夹即可：\n%1").arg(appDir));
        return;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QString::fromUtf8(u8"卸载本程序"));
    box.setText(QString::fromUtf8(u8"确定要卸载「卸载管理器」吗？"));
    box.setInformativeText(QString::fromUtf8(u8"此操作将：\n"
        "• 删除程序文件（%1）\n"
        "• 删除开始菜单快捷方式\n"
        "• 从“设置 ▸ 应用”中移除\n\n"
        "卸载完成后本程序会退出，操作不可撤销。").arg(appDir));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::No);
    if (box.exec() != QMessageBox::Yes) return;

    // 4) 启动独立的卸载桩；它会在自身退出后由 PowerShell 删除整个安装目录。
    //    本程序随后立即退出，释放占用的 Qt DLL / 文件锁，确保 uninst.exe 能顺利删除。
    if (!QProcess::startDetached(uninstPath)) {
        QMessageBox::critical(this, QString::fromUtf8(u8"卸载失败"),
            QString::fromUtf8(u8"无法启动卸载程序：\n%1").arg(uninstPath));
        return;
    }
    QApplication::quit();
}

// 关于对话框：集中展示应用名称与版本号（版本号来自 version.hpp 的 appVersionFull()）。
void UninstallerWindow::showAbout() {
    QMessageBox::about(this,
        QString::fromUtf8(u8"关于 卸载管理器"),
        QString::fromUtf8(u8"卸载管理器\n版本 %1\n\n"
            "一款用于查看、卸载与清理 Windows 已安装软件及残留项的工具。\n"
            "基于 Qt 6 与 C++ 构建。").arg(appVersionFull()));
}

// 启动时的更新日志弹窗：一打开主界面就展示当前版本的更新内容。
// 用户勾选“不再提示此版本”后，该版本不再弹出（基于 QSettings 持久化到注册表）。
void UninstallerWindow::showUpdatePopup() {
    QSettings settings;
    const QString dontShowKey = QStringLiteral("updatePopup/lastShown");
    QString last = settings.value(dontShowKey).toString();
    // 若当前版本已被用户选择“不再提示”，则不弹。
    if (last == appVersionFull()) {
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QString::fromUtf8(u8"卸载管理器 · 更新日志"));
    dlg.setMinimumWidth(480);
    // 弹窗强制白底黑字，不跟随全局亮/暗主题
    dlg.setStyleSheet(
        "QDialog { background-color:#ffffff; }"
        "QLabel { color:#1a1a1a; }"
        "QTextEdit { background-color:#ffffff; color:#1a1a1a; border:1px solid #d0d0d0; border-radius:4px; }"
        "QCheckBox { color:#1a1a1a; }"
        "QCheckBox::indicator:unchecked { border:1px solid #999999; background:#ffffff; }"
        "QPushButton { background-color:#3a6df0; color:#ffffff; border:none; border-radius:6px; padding:8px 20px; font-size:13px; }"
        "QPushButton:hover { background-color:#4f7ef5; }");

    QVBoxLayout* layout = new QVBoxLayout(&dlg);

    QLabel* title = new QLabel(QString::fromUtf8(u8"卸载管理器 %1").arg(appVersionFull()), &dlg);
    title->setStyleSheet("font-size:18px; font-weight:bold; color:#1a1a1a;");
    layout->addWidget(title);

    QLabel* sub = new QLabel(QString::fromUtf8(u8"本次更新内容："), &dlg);
    sub->setStyleSheet("color:#555555;");
    layout->addWidget(sub);

    QTextEdit* te = new QTextEdit(&dlg);
    te->setReadOnly(true);
    te->setPlainText(QString::fromUtf8(
        u8"• ① 新增「运行中」分组：真实检测软件进程状态\n"
        u8"• ② 列表与体积扫描改为多核并行（QtConcurrent），扫描更快不卡顿\n"
        u8"• ③ 修复单字母搜索在排序模式下失效的问题\n"
        u8"• ④ 修复中文软件（如微信）被误判为残留\n"
        u8"• ⑤ 修复带空格路径「打开文件位置」打不开\n"
        u8"• ⑥ 安装位置自动校正（如 WeChat→Weixin 等过时路径）\n"
        u8"• ⑦ 系统组件默认隐藏，可在视图菜单切换显示\n"
        u8"• ⑧ 关闭窗口彻底退出，不再残留进程"
    ));
    te->setFixedHeight(230);
    layout->addWidget(te);

    QCheckBox* cb = new QCheckBox(QString::fromUtf8(u8"不再提示此版本"), &dlg);
    layout->addWidget(cb);

    QPushButton* ok = new QPushButton(QString::fromUtf8(u8"知道了"), &dlg);
    ok->setDefault(true);
    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    btnRow->addWidget(ok);
    layout->addLayout(btnRow);

    QObject::connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
    int rc = dlg.exec();
    if (rc == QDialog::Accepted && cb->isChecked()) {
        settings.setValue(dontShowKey, appVersionFull());
    }
}

void UninstallerWindow::showDetails() {
    showDetailDialog(m_tableWidget->currentRow());
}

// 正经的软件详情对话框：头部图标+名称/版本，表单列出全部信息，
// 底部“功能”区提供 打开文件所在位置 / 卸载 / 扫描残留 / 复制卸载命令。
void UninstallerWindow::showDetailDialog(int row) {
    auto sw = softwareAtRow(row);
    if (!sw) {
        QMessageBox::warning(this, getlang(0x8u).toString(), getlang(0x9u).toString());
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(getlang(0x3Cu).toString());
    dlg.resize(600, 580);

    QVBoxLayout* root = new QVBoxLayout(&dlg);

    // 头部：图标 + 名称/版本
    QHBoxLayout* header = new QHBoxLayout();
    QLabel* iconLabel = new QLabel();
    QPixmap pm = iconForSoftware(sw).pixmap(48, 48);
    if (pm.isNull()) pm = QApplication::style()->standardIcon(QStyle::SP_FileIcon).pixmap(48, 48);
    iconLabel->setPixmap(pm);
    QVBoxLayout* titleBox = new QVBoxLayout();
    QLabel* nameLbl = new QLabel(QString::fromStdString(sw->displayName));
    nameLbl->setStyleSheet("font-size:16px; font-weight:bold;");
    QLabel* verLbl = new QLabel(getlang(0x26).toString() + ": " + QString::fromStdString(sw->displayVersion));
    verLbl->setStyleSheet("color:#9da3ad;");
    titleBox->addWidget(nameLbl);
    titleBox->addWidget(verLbl);
    header->addWidget(iconLabel);
    header->addLayout(titleBox);
    header->addStretch();
    root->addLayout(header);

    // 信息表单
    auto roLine = [](const QString& v) {
        QLineEdit* le = new QLineEdit(v);
        le->setReadOnly(true);
        return le;
    };

    QFormLayout* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(getlang(0x25).toString(), new QLabel(QString::fromStdString(sw->displayName)));
    form->addRow(getlang(0x26).toString(), new QLabel(QString::fromStdString(sw->displayVersion)));
    form->addRow(getlang(0x29).toString(), new QLabel(QString::fromStdString(sw->publisher)));
    form->addRow(getlang(0x27).toString(), new QLabel(QString::fromStdString(sw->installDate)));
    form->addRow(getlang(0x28).toString(), new QLabel(QString::fromStdString(sw->size.get())));

    QString typeStr = sw->isWindowsInstaller ? "Windows Installer (MSI)"
                     : sw->isSystemComponent ? "系统组件" : "普通程序";
    form->addRow("类型", new QLabel(typeStr));
    QString statusStr = sw->isOrphaned ? QString::fromUtf8(u8"残留（卸载程序不存在）") : QString::fromUtf8(u8"正常");
    form->addRow(QString::fromUtf8(u8"状态"), new QLabel(statusStr));
    form->addRow(getlang(0x2A).toString(), roLine(stripQuotes(QString::fromStdString(sw->installLocation))));
    form->addRow("注册表位置", roLine(QString::fromStdString(sw->orgPath)));

    QTextEdit* uninstallEdit = new QTextEdit(stripCommandQuotes(QString::fromStdString(sw->uninstallString)));
    uninstallEdit->setReadOnly(true);
    uninstallEdit->setMaximumHeight(64);
    form->addRow("卸载命令", uninstallEdit);

    if (!sw->helpLink.empty())
        form->addRow(getlang(0x1C).toString(), roLine(QString::fromStdString(sw->helpLink)));
    if (!sw->urlInfoAbout.empty())
        form->addRow(getlang(0x1D).toString(), roLine(QString::fromStdString(sw->urlInfoAbout)));

    root->addLayout(form);

    // 功能按钮区
    QLabel* funcTitle = new QLabel("功能");
    funcTitle->setStyleSheet("font-weight:bold;");
    root->addWidget(funcTitle);

    QGridLayout* funcGrid = new QGridLayout();
    QPushButton* btnOpen = new QPushButton(getlang(0x39).toString());
    QPushButton* btnUninstall = new QPushButton(getlang(0x1F).toString());
    btnUninstall->setObjectName("uninstallBtn");
    QPushButton* btnScan = new QPushButton(getlang(0x20).toString());
    QPushButton* btnCopy = new QPushButton(getlang(0x2F).toString());
    funcGrid->addWidget(btnOpen, 0, 0);
    funcGrid->addWidget(btnUninstall, 0, 1);
    funcGrid->addWidget(btnScan, 1, 0);
    funcGrid->addWidget(btnCopy, 1, 1);
    QPushButton* btnDelReg = new QPushButton(QString::fromUtf8(u8"删除残留注册表项"));
    btnDelReg->setObjectName("uninstallBtn");
    funcGrid->addWidget(btnDelReg, 2, 0, 1, 2);
    QPushButton* btnForceDel = new QPushButton(QString::fromUtf8(u8"强制删除此条目"));
    btnForceDel->setObjectName("uninstallBtn");
    funcGrid->addWidget(btnForceDel, 3, 0, 1, 2);
    root->addLayout(funcGrid);

    // 关闭
    QHBoxLayout* closeBox = new QHBoxLayout();
    closeBox->addStretch();
    QPushButton* btnClose = new QPushButton(getlang(0x3u).toString());
    closeBox->addWidget(btnClose);
    root->addLayout(closeBox);

    connect(btnOpen, &QPushButton::clicked, this, [this, row]() {
        m_tableWidget->setCurrentCell(row, 0);
        openFileLocation();
    });
    connect(btnUninstall, &QPushButton::clicked, &dlg, [this, &dlg, row]() {
        dlg.accept();
        m_tableWidget->setCurrentCell(row, 0);
        uninstallSelected();
    });
    connect(btnScan, &QPushButton::clicked, &dlg, [this, &dlg, row]() {
        dlg.accept();
        m_tableWidget->setCurrentCell(row, 0);
        scanResiduals();
    });
    connect(btnCopy, &QPushButton::clicked, &dlg, [this, &dlg, row]() {
        dlg.accept();
        m_tableWidget->setCurrentCell(row, 0);
        copyUninstallCommand();
    });
    connect(btnDelReg, &QPushButton::clicked, &dlg, [this, &dlg, row]() {
        dlg.accept();
        m_tableWidget->setCurrentCell(row, 0);
        deleteRegistryEntry();
    });
    connect(btnForceDel, &QPushButton::clicked, &dlg, [this, &dlg, row]() {
        dlg.accept();
        m_tableWidget->setCurrentCell(row, 0);
        forceDeleteEntry();
    });
    connect(btnClose, &QPushButton::clicked, &dlg, &QDialog::accept);

    dlg.exec();
}

// 前向声明：openFileLocation 会用到，定义在文件下方（静态辅助函数）。
static QString extractExePath(const QString& cmd);

void UninstallerWindow::openFileLocation() {
    int row = m_tableWidget->currentRow();
    if (tick(row)) return;
    auto sw = softwareAtRow(row);

    QString target;
    bool selectFile = false; // true: explorer /select,<file> 选中具体文件；false: 打开文件夹

    // 优先从卸载命令定位到具体的可执行文件（如 FeverGamesLauncher.exe），
    // 这样“打开文件所在位置”会真正选中该 exe，而不是只打开安装目录。
    QString cmd = QString::fromStdString(Registry::getUninstallCommand(*sw));
    QString exe = extractExePath(cmd);
    if (!exe.isEmpty() && exe.contains("\\") &&
        !exe.contains("msiexec", Qt::CaseInsensitive) &&
        QFile::exists(exe)) {
        target = exe;
        selectFile = true;
    }

    // 若无法定位到 exe 文件，再回退到安装目录
    if (target.isEmpty()) {
        QString installLoc = stripQuotes(QString::fromStdString(sw->installLocation)).trimmed();
        if (!installLoc.isEmpty() && QDir(installLoc).exists()) {
            target = installLoc;
            selectFile = false;
        }
    }

    // 安装目录也没有：再退而求其次，打开卸载命令中 exe 所在的文件夹
    if (target.isEmpty() && !exe.isEmpty() && exe.contains("\\") &&
        !exe.contains("msiexec", Qt::CaseInsensitive)) {
        QFileInfo fi(exe);
        if (fi.isAbsolute() && !fi.absolutePath().isEmpty()) {
            target = fi.absolutePath();
            selectFile = false;
        }
    }

    // 最终校验：target 为空或实际不存在时，直接提示“找不到文件位置”，
    // 不要调用 explorer，避免 Windows 把无效路径解释成“文档”夹。
    if (target.isEmpty() || !QFileInfo(target).exists()) {
        QMessageBox::warning(this, getlang(0xFu).toString(), getlang(0x3B).toString());
        return;
    }

    // 调用资源管理器：文件用 /select 高亮，文件夹直接打开。
    // 改用 ShellExecuteW 直接构造命令参数字符串，避免 QProcess::startDetached 在 Windows
    // 上对 /select,<path> 中含空格路径的参数加引号方式与 explorer 解析不兼容，
    // 导致资源管理器打开默认“文档”夹而非目标位置。
    QString native = QDir::toNativeSeparators(target);
    QString params;
    if (selectFile) {
        params = QString("/select,\"%1\"").arg(native);
    } else {
        params = QString("/e,\"%1\"").arg(native);
    }
    auto ret = reinterpret_cast<intptr_t>(::ShellExecuteW(
        nullptr, L"open", L"explorer.exe",
        params.toStdWString().c_str(), nullptr, SW_SHOWNORMAL));
    if (ret <= 32) {
        QMessageBox::warning(this, getlang(0xFu).toString(), getlang(0x3B).toString());
    }
}

void UninstallerWindow::toggleShowSystemComponents() {
    m_showSystemComponents = !m_showSystemComponents;
    if (m_showSystemAction) {
        m_showSystemAction->setChecked(m_showSystemComponents);
    }
    updateFindList();
    loadSoftwareList();
}

void UninstallerWindow::exportSoftwareList() {
    QString fileName = QFileDialog::getSaveFileName(
        this,
        getlang(0x2E).toString(),
        "software_list.csv",
        QString::fromUtf8(u8"CSV 文件 (*.csv);;HTML 文件 (*.html);;文本文件 (*.txt);;所有文件 (*)"));
    if (fileName.isEmpty()) return;

    // 收集当前可见行数据
    struct Row { QString name, ver, pub, date, size, loc, status; };
    QVector<Row> rows;
    for (int i = 0; i < m_tableWidget->rowCount(); ++i) {
        if (m_tableWidget->isRowHidden(i)) continue;
        auto sw = softwareAtRow(i);
        if (!sw) continue;
        rows.append({ QString::fromStdString(sw->displayName),
                      QString::fromStdString(sw->displayVersion),
                      QString::fromStdString(sw->publisher),
                      QString::fromStdString(sw->installDate),
                      QString::fromStdString(sw->size.get()),
                      QString::fromStdString(sw->installLocation),
                      sw->isOrphaned ? QString::fromUtf8(u8"残留") : QString::fromUtf8(u8"正常") });
    }

    QString lower = fileName.toLower();
    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, getlang(0x2Eu).toString(), QString::fromUtf8(u8"无法导出文件，请检查保存路径与写入权限。"));
        return;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    if (lower.endsWith(".html")) {
        out << "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
               "<title>软件清单</title><style>"
               "table{border-collapse:collapse;font-family:sans-serif;font-size:13px}"
               "th,td{border:1px solid #ccc;padding:4px 8px;text-align:left}"
               "th{background:#f0f0f0}tr:nth-child(even){background:#fafafa}</style></head><body>"
               "<h2>已安装软件清单</h2><table><thead><tr>"
               "<th>名称</th><th>版本</th><th>发行商</th><th>安装日期</th><th>大小</th><th>安装位置</th><th>状态</th>"
               "</tr></thead><tbody>\n";
        for (const auto& r : rows) {
            // 仅用 toHtmlEscaped() 做转义；不要再手工替换 & < >，否则会与
            // toHtmlEscaped 内部转义叠加造成双重转义（浏览器里 & 会变成字面 &amp;）。
            auto esc = [](const QString& s) { return s.toHtmlEscaped(); };
            out << "<tr><td>" << esc(r.name) << "</td><td>" << esc(r.ver) << "</td><td>"
                << esc(r.pub) << "</td><td>" << esc(r.date) << "</td><td>" << esc(r.size)
                << "</td><td>" << esc(r.loc) << "</td><td>" << esc(r.status) << "</td></tr>\n";
        }
        out << "</tbody></table></body></html>";
    } else if (lower.endsWith(".csv")) {
        // 写 UTF-8 BOM，使 Excel 能正确识别中文（否则默认按 ANSI 解析会乱码）。
        out.setGenerateByteOrderMark(true);
        // CSV：字段含逗号/引号/换行时按 RFC4180 用双引号包裹并转义内部引号。
        auto csvCell = [](const QString& s) -> QString {
            QString cell = s;
            if (cell.contains(',') || cell.contains('"') || cell.contains('\n') || cell.contains('\r')) {
                cell.replace('"', "\"\"");
                return "\"" + cell + "\"";
            }
            return cell;
        };
        out << "Name,Version,Publisher,InstallDate,Size,Location,Status\n";
        for (const auto& r : rows) {
            out << csvCell(r.name) << "," << csvCell(r.ver) << "," << csvCell(r.pub) << ","
                << csvCell(r.date) << "," << csvCell(r.size) << "," << csvCell(r.loc) << ","
                << csvCell(r.status) << "\n";
        }
    } else {
        // 文本/TSV 也写 UTF-8 BOM，避免 Excel 打开中文乱码。
        out.setGenerateByteOrderMark(true);
        out << "Name\tVersion\tPublisher\tInstallDate\tSize\tLocation\tStatus\n";
        for (const auto& r : rows) {
            out << r.name << "\t" << r.ver << "\t" << r.pub << "\t" << r.date << "\t"
                << r.size << "\t" << r.loc << "\t" << r.status << "\n";
        }
    }
    file.close();
    QMessageBox::information(this, getlang(0x31).toString(), getlang(0x32).toString().arg(fileName));
}

void UninstallerWindow::copyUninstallCommand() {
    int row = m_tableWidget->currentRow();
    if (tick(row)) return;

    auto sw = softwareAtRow(row);
    QString cmd = QString::fromStdString(sw->uninstallString);
    if (cmd.isEmpty()) {
        cmd = getlang(0x37).toString();
    }

    QApplication::clipboard()->setText(cmd);
    QMessageBox::information(this, getlang(0x33).toString(), getlang(0x34).toString());
}

void UninstallerWindow::showDevInfo() {
    sorting();
    int counts[5] = { 0, 0, 0, 0, 0 };
    for (const auto& pair : m_swlist) {
        counts[pair.first] = static_cast<int>(pair.second.size());
    }

    QString info = getlang(0x36).toString()
        .arg(qVersion())
        .arg("Clang 17.0.6 (llvm-mingw1706)")
        .arg("Release")
        .arg(m_softwareList.size())
        .arg(counts[0])
        .arg(counts[1])
        .arg(counts[2])
        .arg(counts[3])
        .arg(counts[4]);

    QMessageBox::information(this, getlang(0x35).toString(), info);
}

// 取中文串的拼音首字母（用于搜索：搜 "wx" 也能命中“微信”）。
// 覆盖常见软件名用字；不在表中的汉字直接跳过，避免产生无意义字母。
// 写成自由函数，供下方的 buildHaystack（同为自由函数）调用。
static QString pinyinInitials(const QString& s) {
    static const QHash<QChar, QChar> kMap = {
        {QChar(0x5FAE), 'w'}, {QChar(0x4FE1), 'x'}, {QChar(0x817E), 't'}, {QChar(0x8BAF), 'x'},
        {QChar(0x7F51), 'w'}, {QChar(0x6613), 'y'}, {QChar(0x963F), 'a'}, {QChar(0x91CC), 'l'},
        {QChar(0x767E), 'b'}, {QChar(0x5EA6), 'd'}, {QChar(0x9489), 'd'}, {QChar(0x54D4), 'b'},
        {QChar(0x5496), 'l'}, {QChar(0x7231), 'a'}, {QChar(0x5947), 'q'}, {QChar(0x9177), 'k'},
        {QChar(0x72D7), 'g'}, {QChar(0x641C), 's'}, {QChar(0x5C71), 's'}, {QChar(0x91D1), 'j'},
        {QChar(0x4F18), 'y'}, {QChar(0x6DD8), 't'}, {QChar(0x4EAC), 'j'}, {QChar(0x4E1C), 'd'},
        {QChar(0x7F8E), 'm'}, {QChar(0x56E2), 't'}, {QChar(0x6296), 'd'}, {QChar(0x97F3), 'y'},
        {QChar(0x5FEB), 'k'}, {QChar(0x624B), 's'}, {QChar(0x8F93), 's'}, {QChar(0x6B4C), 'g'},
        {QChar(0x6E38), 'y'}, {QChar(0x620F), 'x'}, {QChar(0x89C6), 's'}, {QChar(0x9891), 'p'},
        {QChar(0x79D1), 'k'}, {QChar(0x6280), 'j'}, {QChar(0x8F6F), 'r'}, {QChar(0x4EF6), 'j'},
        {QChar(0x5DE5), 'g'}, {QChar(0x4F5C), 'z'}, {QChar(0x5BA4), 's'}, {QChar(0x5B89), 'a'},
        {QChar(0x88C5), 'z'}, {QChar(0x7BA1), 'g'}, {QChar(0x7406), 'l'}, {QChar(0x89E3), 'j'},
        {QChar(0x538B), 'y'}, {QChar(0x6D4F), 'l'}, {QChar(0x89C8), 'l'}, {QChar(0x5668), 'q'},
        {QChar(0x5168), 'q'}, {QChar(0x6740), 's'}, {QChar(0x6BD2), 'd'}, {QChar(0x4E91), 'y'},
        {QChar(0x76D8), 'p'}, {QChar(0x4E50), 'l'}, {QChar(0x56FE), 't'}, {QChar(0x7247), 'p'},
        {QChar(0x6587), 'w'}, {QChar(0x6863), 'd'}, {QChar(0x793E), 's'}, {QChar(0x4EA4), 'j'},
        {QChar(0x8D2D), 'g'}, {QChar(0x7269), 'w'}, {QChar(0x76F4), 'z'}, {QChar(0x64AD), 'b'},
        {QChar(0x5F71), 'y'}, {QChar(0x7535), 'd'}, {QChar(0x5E94), 'y'}, {QChar(0x7528), 'y'},
        {QChar(0x7F16), 'b'}, {QChar(0x8F91), 'j'}, {QChar(0x8BBE), 's'}, {QChar(0x8BA1), 'j'},
        {QChar(0x7ED8), 'h'}, {QChar(0x753B), 'h'}, {QChar(0x526A), 'j'}, {QChar(0x5F55), 'l'},
        {QChar(0x5C4F), 'p'}, {QChar(0x622A), 'j'}, {QChar(0x7F29), 's'}, {QChar(0x5305), 'b'},
        {QChar(0x4E0B), 'x'}, {QChar(0x8F7D), 'z'}, {QChar(0x5177), 'j'}, {QChar(0x9A71), 'q'},
        {QChar(0x52A8), 'd'}, {QChar(0x7A0B), 'c'}, {QChar(0x5E8F), 'x'}, {QChar(0x7CFB), 'x'},
        {QChar(0x7EDF), 't'}, {QChar(0x64CD), 'c'}, {QChar(0x66F4), 'g'}, {QChar(0x65B0), 'x'},
        {QChar(0x5Fae), 'w'}, {QChar(0x4fe1), 'x'}, {QChar(0x817e), 't'}, {QChar(0x8baf), 'x'},
        {QChar(0x8ba1), 'j'}, {QChar(0x7ed8), 'h'},
    };
    QString out;
    for (QChar ch : s) {
        if (kMap.contains(ch)) out.append(kMap[ch]);
    }
    return out;
}

// 判断是否为“系统关键项”（Windows 更新 / 驱动 / 系统组件等），
// 这类项一旦误卸载会导致系统不稳定，故在卸载/删残留前拦截并提示。
bool UninstallerWindow::isCriticalSystemItem(const SoftwareInfo* sw) const {
    if (!sw) return false;
    // 注册表已标记的 SystemComponent 直接视为系统组件
    if (sw->isSystemComponent) return true;
    QString name = QString::fromStdString(sw->displayName).toLower();
    QString pub = QString::fromStdString(sw->publisher).toLower();
    // 发行商黑名单：微软 / 系统级厂商
    if (pub.contains("microsoft") || pub.contains("windows") ||
        pub.contains("intel") || pub.contains("advanced micro devices") ||
        pub.contains("realtek") || pub.contains("qualcomm") ||
        pub.contains("oem") || pub.contains("canonical") ||
        pub.contains("apple inc")) {
        // 仅对“明显是驱动/更新/运行时”的条目拦截，避免误伤微软正常应用
        if (name.contains("driver") || name.contains("更新") || name.contains("update") ||
            name.contains("hotfix") || name.contains("redistributable") ||
            name.contains("visual c++") || name.contains("runtime") ||
            name.contains("driver") || name.contains("kb") ||
            name.contains("windows") || name.contains("component") ||
            name.contains("service pack") || name.contains("security")) {
            return true;
        }
    }
    // 名称特征：KB 补丁号、Windows 组件、驱动
    if (name.contains("kb") && name.contains("update")) return true;
    if (name.contains("windows driver") || name.contains("设备驱动")) return true;
    if (name.contains("microsoft visual c++") || name.contains("microsoft .net")) return true;
    return false;
}


// 把搜索文本统一小写，并把希腊字母 μ 替换成拉丁字母 u，
// 这样用户输入 "uvision" 也能匹配注册表里的 "μVision"。
// 从卸载命令中提取可执行文件路径（去掉引号、取第一个空白前的内容）。
// 例如 "C:\app\uninst.exe" /S -> C:\app\uninst.exe
static QString extractExePath(const QString& cmd) {
    if (cmd.isEmpty()) return QString();
    QString s = cmd.trimmed();
    // 带引号的命令："C:\Program Files\App\uninst.exe" /S
    if (s.startsWith('"')) {
        int end = s.indexOf('"', 1);
        if (end != -1) return s.mid(1, end - 1);
        return s.mid(1);
    }
    // 无引号命令：C:\Program Files\App\uninst.exe /S
    // 必须按 .exe 截取，而不是按第一个空格截取，否则带空格路径会被截断。
    int exePos = s.indexOf(".exe", 0, Qt::CaseInsensitive);
    if (exePos != -1) {
        int end = exePos + 4; // 包含 ".exe"
        int sp = s.indexOf(' ', end);
        return (sp == -1) ? s : s.left(sp);
    }
    // 没有 .exe（如 msiexec 或错误命令）
    int sp = s.indexOf(' ');
    return (sp == -1) ? s : s.left(sp);
}

// 归一化用于搜索：转小写、μ/µ -> u，并去掉空白与常见分隔符，
// 使 “Visual Studio” 与 “visualstudio”、“node.js” 与 “nodejs” 等价匹配。
static QString compactForSearch(const QString& s) {
    QString r = s.toLower();
    r.replace(QChar(0x03BC), 'u');   // 希腊小写字母 mu (μ)
    r.replace(QChar(0x00B5), 'u');   // 微符号 (µ)
    QString out;
    for (QChar ch : r) {
        if (ch.isSpace()) continue;
        if (ch == QChar('_') || ch == QChar('-') || ch == QChar('.') ||
            ch == QChar(',') || ch == QChar('/') || ch == QChar('\\') ||
            ch == QChar('(') || ch == QChar(')') ||
            ch == QChar('[') || ch == QChar(']')) continue;
        out.append(ch);
    }
    return out;
}

// 别名 / 拼音匹配：常见中文软件名 ↔ 英文名的等价组。
// 例如搜“微信”能匹配显示名为 “WeChat” 的条目，反之亦然。
// 组与组之间相互独立；一个词只会命中自己所在组的成员。
static const QStringList kAliasGroups[] = {
    {QString::fromUtf8(u8"微信"), QStringLiteral("wechat"), QStringLiteral("weixin")},
    {QString::fromUtf8(u8"腾讯"), QStringLiteral("tencent")},
    {QString::fromUtf8(u8"qq"),   QStringLiteral("oicq")},
    {QString::fromUtf8(u8"网易"), QStringLiteral("netease")},
    {QString::fromUtf8(u8"阿里"), QStringLiteral("alibaba"), QStringLiteral("aliyun")},
    {QString::fromUtf8(u8"百度"), QStringLiteral("baidu")},
    {QString::fromUtf8(u8"支付宝"), QStringLiteral("alipay")},
    {QString::fromUtf8(u8"钉钉"), QStringLiteral("dingtalk")},
    {QString::fromUtf8(u8"哔哩哔哩"), QStringLiteral("bilibili"), QString::fromUtf8(u8"b站")},
    {QString::fromUtf8(u8"爱奇艺"), QStringLiteral("iqiyi")},
    {QString::fromUtf8(u8"酷狗"), QStringLiteral("kugou")},
    {QString::fromUtf8(u8"迅雷"), QStringLiteral("thunder"), QStringLiteral("xunlei")},
    {QString::fromUtf8(u8"搜狗"), QStringLiteral("sogou")},
    {QStringLiteral("360"), QStringLiteral("qihu")},
    {QString::fromUtf8(u8"金山"), QStringLiteral("kingsoft")},
    {QString::fromUtf8(u8"优酷"), QStringLiteral("youku")},
    {QString::fromUtf8(u8"淘宝"), QStringLiteral("taobao")},
    {QString::fromUtf8(u8"京东"), QStringLiteral("jd"), QString::fromUtf8(u8"jingdong")},
    {QString::fromUtf8(u8"美团"), QStringLiteral("meituan")},
    {QString::fromUtf8(u8"抖音"), QStringLiteral("douyin"), QStringLiteral("tiktok")},
    {QString::fromUtf8(u8"快手"), QStringLiteral("kuaishou"), QStringLiteral("kwai")},
    {QString::fromUtf8(u8"微信输入法"), QStringLiteral("wechatinput")},
};

// 返回某个词的“等价词集合”（含自身）：让中文查询命中英文显示名，反之亦然。
// term 应为已归一化（小写、去分隔符）的词，否则可能匹配不到组。
static QStringList synonymSet(const QString& term) {
    QStringList out{term};
    for (const auto& g : kAliasGroups) {
        if (g.contains(term)) {
            for (const auto& s : g) {
                if (!out.contains(s)) out.append(s);
            }
        }
    }
    return out;
}

// 把字符串拆成可用于搜索的“词”：先按空白与常见分隔符切分，再 compact。
// 这样 "WeChat for Windows" 能得到 wechat / for / windows 三个 token。
static QStringList searchTokens(const QString& s) {
    QStringList out;
    QString cur;
    for (QChar ch : s) {
        if (ch.isSpace() || ch == QChar('_') || ch == QChar('-') || ch == QChar('.') ||
            ch == QChar(',') || ch == QChar('/') || ch == QChar('\\') ||
            ch == QChar('(') || ch == QChar(')') ||
            ch == QChar('[') || ch == QChar(']')) {
            if (!cur.isEmpty()) { out.append(compactForSearch(cur)); cur.clear(); }
        } else {
            cur.append(ch);
        }
    }
    if (!cur.isEmpty()) out.append(compactForSearch(cur));
    return out;
}

// 把可搜索字段拼成一个归一化串（软件名 + 发行商 + 版本号），
// 这样搜索不再只限于软件名称，厂商名 / 版本号也能搜到（不含安装路径）。
// 同时把软件名 / 发行商自身的别名并入 haystack，使搜中文名也能命中英文名条目。
static QString buildHaystack(const SoftwareInfo* sw) {
    QString name = QString::fromStdString(sw->displayName);
    QString pub  = QString::fromStdString(sw->publisher);
    QString ver  = QString::fromStdString(sw->displayVersion);

    QStringList parts;
    parts << name << pub << ver;

    // 把软件名 / 发行商自身的别名也追加进 haystack。
    // 注意：必须按“词”去查别名表，不能对整个 compact 后的长串查，
    // 否则 "WeChat for Windows" / "Tencent Technology" 这类带额外单词的
    // 名称永远匹配不到 "微信" / "腾讯" 等价组。
    for (const QString& n : {name, pub}) {
        if (n.isEmpty()) continue;
        // 整个字段的 compact 也加进来，保留完整连续匹配能力。
        QString whole = compactForSearch(n);
        if (!whole.isEmpty()) {
            for (const QString& syn : synonymSet(whole)) {
                if (!parts.contains(syn)) parts.append(syn);
            }
        }
        // 再按词切分，逐个查别名。
        for (const QString& tk : searchTokens(n)) {
            if (tk.isEmpty()) continue;
            for (const QString& syn : synonymSet(tk)) {
                if (!parts.contains(syn)) parts.append(syn);
            }
        }
        // ⑤ 拼音首字母：把中文软件名的首字母（如“微信”→wx）也并入 haystack，
        // 这样搜 "wx" 能命中“微信”、搜 "tx" 能命中“腾讯”。
        QString py = pinyinInitials(n).toLower();
        if (!py.isEmpty() && !parts.contains(py)) parts.append(py);
    }
    return compactForSearch(parts.join(QChar(' ')));
}

void UninstallerWindow::filterSoftware() {
    // 多关键词：按空白拆分，所有词都需命中（AND），提升精度并容忍大小写/空格差异。
    QString raw = m_searchEdit->text().trimmed();
    QStringList tokens = raw.simplified().split(QChar(' '), Qt::SkipEmptyParts);
    QStringList compactTokens;
    for (const QString& t : tokens) {
        QString c = compactForSearch(t);
        if (!c.isEmpty()) compactTokens.append(c);
    }

    // setRowHidden() 在排序启用时与排序代理的 visual/logical 行映射交互不可靠，
    // 会表现为隐藏/显示错乱、过滤不生效。因此：搜索词非空时彻底关闭排序；
    // 清空搜索词后再恢复排序。这样隐藏状态在稳定环境下工作。
    bool wantSorting = compactTokens.isEmpty();
    if (m_tableWidget->isSortingEnabled() != wantSorting) {
        m_tableWidget->setSortingEnabled(wantSorting);
        if (wantSorting) {
            m_tableWidget->sortItems(0, Qt::AscendingOrder);
        }
    }

    int visibleCount = 0;
    filesize_t visibleSize;

    for (int i = 0; i < m_tableWidget->rowCount(); ++i) {
        auto sw = softwareAtRow(i);
        bool match = false;
        if (sw) {
            QString hay = buildHaystack(sw);
            bool nameOk = true; // 空搜索词时循环不执行，保持 true -> 全部显示
            for (const QString& tk : compactTokens) {
                // 把每个关键词扩展成等价词集合（如“微信”→微信/wechat/weixin），
                // 只要 haystack 命中其中任意一个即视为匹配，实现中英文互搜。
                bool km = false;
                for (const QString& syn : synonymSet(tk)) {
                    QString cs = compactForSearch(syn);
                    if (hay.contains(cs)) { km = true; break; }
                }
                // ⑤ 前缀匹配：搜 "wech" 也能命中 "wechat for windows"（hay 以 token 开头）。
                if (!km && hay.startsWith(tk)) { km = true; }
                if (!km) { nameOk = false; break; }
            }
            // “仅显示残留项”过滤：开启时只保留 isOrphaned 的条目
            bool orphanOk = !m_showOrphanOnly || sw->isOrphaned;
            match = nameOk && orphanOk;
        }
        m_tableWidget->setRowHidden(i, !match);
        if (match) {
            ++visibleCount;
            if (sw) visibleSize += sw->size.size;
        }
    }

    statusBar()->showMessage(getlang(0x7u).toString().arg(visibleCount).arg(QString::fromStdString(visibleSize.get())));
}

void UninstallerWindow::loadLanguageSetting() {
    // 从 QSettings（Windows 下写入注册表）恢复上次选择的语言；非法或缺失则保持默认。
    QSettings s(QStringLiteral("Uninstaller"), QStringLiteral("uninstaller"));
    bool ok = false;
    int v = s.value(QStringLiteral("language"), -1).toInt(&ok);
    if (ok && v >= 0 && v < langCount()) {
        G.LANGUAGE = static_cast<short>(v);
    }
}

// 按 family 一级大区构建语言二级菜单（一级 = 地理大区，二级 = 语系，用分隔线分组）。
// 只在 setupUI() 里调用一次；之后语言列表不会动态变化，无需重建。
// m_langActions 与 langCount() 一一对应（按 langIndex 索引），方便 setLanguage 同步勾选状态。
//
// 顺序：菜单项顺序按 languages.json 里 families 数组的首次出现顺序（group 由
// langFamilyGroups() 按 families first-appearance 去重得出）；同大区内按系族
// 插入分隔线。family 完全为空的语言单列进独立的"其他"兜底菜单——但只要
// JSON 没填空的 family，这种情况不会触发。
void UninstallerWindow::buildLanguageMenuItems() {
    if (!m_langMenu) return;
    const int n = langCount();

    m_langActions.assign(n, nullptr);

    QActionGroup* langGroup = new QActionGroup(this);
    langGroup->setExclusive(true);

    const QStringList groups = langFamilyGroups();
    if (groups.isEmpty()) {
        for (int i = 0; i < n; ++i) {
            QAction* a = m_langMenu->addAction(langName(i, G.LANGUAGE));
            a->setCheckable(true);
            a->setChecked(i == G.LANGUAGE);
            a->setActionGroup(langGroup);
            connect(a, &QAction::triggered, this, [this, i]() { setLanguage(i); });
            m_langActions[i] = a;
        }
        return;
    }

    // 按一级大区 (primary) 归类；family 完全为空的进 unfam 兜底
    QHash<QString, QList<int>> byGroup;
    QList<int> unfam;
    for (int i = 0; i < n; ++i) {
        const QString fam = langFamily(i);                          // e.g. "欧洲 Germanic"
        if (fam.trimmed().isEmpty()) {
            unfam.append(i);
            continue;
        }
        const QString primary = fam.section(QChar(' '), 0, 0);      // "欧洲"
        byGroup[primary].append(i);
    }

    // 主循环：按 families 数组里 primary 的首次出现顺序建菜单；
    // groups 已按 first-appearance 去重（包括 Esperanto/Latin 的 "其他"），
    // 所以"其他"只在这里 addMenu 一次。
    for (const QString& primary : groups) {
        if (byGroup.value(primary).isEmpty()) continue;             // 该 primary 下没有语言就跳过
        QMenu* sub = m_langMenu->addMenu(primary);
        const QList<int>& idxs = byGroup.value(primary);
        QString prevSubFamily;
        for (int idx : idxs) {
            const QString subFam = langFamily(idx).section(QChar(' '), 1);   // "Germanic"
            if (!subFam.isEmpty() && !prevSubFamily.isEmpty() && subFam != prevSubFamily) {
                sub->addSeparator();
            }
            QAction* a = sub->addAction(langName(idx, G.LANGUAGE));
            a->setCheckable(true);
            a->setChecked(idx == G.LANGUAGE);
            a->setActionGroup(langGroup);
            connect(a, &QAction::triggered, this, [this, idx]() { setLanguage(idx); });
            m_langActions[idx] = a;
            if (!subFam.isEmpty()) prevSubFamily = subFam;
        }
    }

    // 空 family 兜底菜单（正常 JSON 下应当为空，避免重复"其他"的关键就在这里）
    if (!unfam.isEmpty()) {
        QMenu* other = m_langMenu->addMenu(QStringLiteral("其他"));
        for (int idx : unfam) {
            QAction* a = other->addAction(langName(idx, G.LANGUAGE));
            a->setCheckable(true);
            a->setChecked(idx == G.LANGUAGE);
            a->setActionGroup(langGroup);
            connect(a, &QAction::triggered, this, [this, idx]() { setLanguage(idx); });
            m_langActions[idx] = a;
        }
    }
}

void UninstallerWindow::setLanguage(int lang) {
    if (lang < 0 || lang >= langCount()) lang = 1; // 越界回落到中文
    G.LANGUAGE = static_cast<short>(lang);

    QSettings s(QStringLiteral("Uninstaller"), QStringLiteral("uninstaller"));
    s.setValue(QStringLiteral("language"), G.LANGUAGE);

    if (m_langCombo) m_langCombo->setCurrentIndex(G.LANGUAGE);
    // 同步菜单勾选状态：使用 m_langActions 直接按 langIndex 定位，避免遍历嵌套子菜单。
    for (size_t i = 0; i < m_langActions.size(); ++i) {
        if (m_langActions[i]) m_langActions[i]->setChecked(static_cast<int>(i) == G.LANGUAGE);
    }

    retranslateUI();
}

void UninstallerWindow::retranslateUI() {
    // 窗口标题（与 setupUI 中格式保持一致）
    setWindowTitle(getlang(0x15).toString() + " " + appVersionFull() + " " + QStringLiteral(__DATE__));

    // 菜单栏
    if (actionMenu) {
        actionMenu->setTitle(getlang(0x0).toString());
        const QList<QAction*> acts = actionMenu->actions();
        if (acts.size() > 0) acts[0]->setText(getlang(0x1f).toString());
        if (acts.size() > 1) acts[1]->setText(getlang(0x20).toString());
        if (acts.size() > 2) acts[2]->setText(getlang(0x21).toString());
    }
    if (m_selfMenu) {
        m_selfMenu->setTitle(getlang(0x3E).toString());
        if (m_selfUninstallAction) m_selfUninstallAction->setText(getlang(0x3F).toString());
        if (m_aboutAction) m_aboutAction->setText(getlang(0x40).toString());
    }
    if (m_devMenu) {
        m_devMenu->setTitle(getlang(0x2C).toString());
        if (m_showSystemAction) m_showSystemAction->setText(getlang(0x2D).toString());
        const QList<QAction*> dacts = m_devMenu->actions();
        // 顺序：0 显示系统组件 / 1 导出列表 / 2 复制命令 / 3 调试信息（1 为分隔符后的位置）
        if (dacts.size() > 1) dacts[1]->setText(getlang(0x2E).toString());
        if (dacts.size() > 2) dacts[2]->setText(getlang(0x2F).toString());
        if (dacts.size() > 3) dacts[3]->setText(getlang(0x30).toString());
    }
    if (m_langMenu) m_langMenu->setTitle(getlang(0x3D).toString());
    // 语言菜单项随当前界面语言翻译：用 langName(idx, G.LANGUAGE) 改写每个动作文本。
    // （大区分组标题来自 families，保持中文不变。）
    for (size_t i = 0; i < m_langActions.size(); ++i) {
        if (m_langActions[i]) m_langActions[i]->setText(langName(static_cast<int>(i), G.LANGUAGE));
    }
    // 语言下拉框同步翻译每个条目
    if (m_langCombo) {
        for (int i = 0; i < langCount(); ++i)
            m_langCombo->setItemText(i, langName(i, G.LANGUAGE));
    }

    // 工具栏
    if (m_scanLabel) m_scanLabel->setText(getlang(0x24).toString());
    if (m_searchEdit) m_searchEdit->setPlaceholderText(getlang(0x22).toString());
    if (m_refreshBtn) m_refreshBtn->setText(getlang(0x23).toString());
    if (m_orphanOnlyCheck) m_orphanOnlyCheck->setText(QString::fromUtf8(u8"仅显示残留项"));

    // 表格表头
    if (m_tableWidget) {
        QStringList headers = {
            getlang(0x25).toString(), getlang(0x26).toString(), getlang(0x27).toString(),
            getlang(0x28).toString(), getlang(0x29).toString(), getlang(0x2A).toString(),
            getlang(0x42).toString()
        };
        m_tableWidget->setHorizontalHeaderLabels(headers);
    }

    // 按钮栏
    if (m_uninstallBtn) m_uninstallBtn->setText(getlang(0x1f).toString());
    if (m_scanBtn) m_scanBtn->setText(getlang(0x20).toString());
    if (m_detailsBtn) m_detailsBtn->setText(getlang(0x21).toString());

    // 状态栏：用新语言重算“共找到 N 个软件，共占 ...”提示
    filterSoftware();
}

void UninstallerWindow::setupUI() {
    // 幂等：避免 run()/fresh() 多次调用时重复创建菜单与表格导致内存泄漏。
    if (m_uiBuilt) return;
    m_uiBuilt = true;

    // 样式统一由 setTheme() 通过 qApp->setStyleSheet 管理，避免此处硬编码覆盖主题。

    // 标题栏附带编译日期，方便区分本地旧 exe 与新构建。
    setWindowTitle(getlang(0x15).toString() + " " + appVersionFull() +
                   " " + QStringLiteral(__DATE__));
    resize(G.WINDOWS_SIZE[0], G.WINDOWS_SIZE[1]);

    QWidget* centralWidget = new QWidget(this);
    centralWidget->setObjectName("centralWidget");
    setCentralWidget(centralWidget);

    QVBoxLayout* mainLayout = new QVBoxLayout(centralWidget);

    // 菜单栏
    bar = menuBar();
    actionMenu = bar->addMenu(getlang(0x0).toString());
    actionMenu->addAction(getlang(0x1f).toString(), this, &UninstallerWindow::uninstallSelected);
    actionMenu->addAction(getlang(0x20).toString(), this, &UninstallerWindow::scanResiduals);
    actionMenu->addAction(getlang(0x21).toString(), this, &UninstallerWindow::showDetails);

    // “本程序”菜单：提供卸载自身的能力（区别于卸载列表中的其他软件）。
    m_selfMenu = bar->addMenu(getlang(0x3E).toString());
    m_selfUninstallAction = m_selfMenu->addAction(getlang(0x3F).toString(), this, &UninstallerWindow::uninstallSelf);
    m_selfMenu->addSeparator();
    m_aboutAction = m_selfMenu->addAction(getlang(0x40).toString(), this, &UninstallerWindow::showAbout);

    // 语言切换菜单：按"地理/语系"大区分组为二级 QMenu（108 项扁平展开已超出屏幕可控范围）。
    // 语言名保持各自母语写法，不随界面翻译。Family 数据来自 languages.json 的 families 字段，
    // 缺失则全部归入"其他"。
    m_langMenu = bar->addMenu(getlang(0x3D).toString());
    buildLanguageMenuItems();
    // 初始化勾选状态（让持久化的 G.LANGUAGE 在菜单里立刻可见）。
    if (G.LANGUAGE >= 0 && G.LANGUAGE < static_cast<int>(m_langActions.size())) {
        if (m_langActions[G.LANGUAGE]) m_langActions[G.LANGUAGE]->setChecked(true);
    }

    m_devMenu = bar->addMenu(getlang(0x2C).toString());
    m_showSystemAction = m_devMenu->addAction(getlang(0x2D).toString(), this, &UninstallerWindow::toggleShowSystemComponents);
    m_showSystemAction->setCheckable(true);
    m_showSystemAction->setChecked(m_showSystemComponents);
    m_devMenu->addAction(getlang(0x2E).toString(), this, &UninstallerWindow::exportSoftwareList);
    m_devMenu->addAction(getlang(0x2F).toString(), this, &UninstallerWindow::copyUninstallCommand);
    m_devMenu->addAction(getlang(0x30).toString(), this, &UninstallerWindow::showDevInfo);

    // ⑨ 视图/主题菜单
    m_viewMenu = bar->addMenu(QString::fromUtf8(u8"视图"));
    m_lightThemeAct = m_viewMenu->addAction(QString::fromUtf8(u8"亮色主题"));
    m_darkThemeAct = m_viewMenu->addAction(QString::fromUtf8(u8"暗色主题"));
    // 互斥单选：同一时刻只能勾一个（避免“亮/暗同时选中”）
    QActionGroup* themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);
    m_lightThemeAct->setActionGroup(themeGroup);
    m_darkThemeAct->setActionGroup(themeGroup);
    m_lightThemeAct->setCheckable(true);
    m_darkThemeAct->setCheckable(true);
    connect(m_lightThemeAct, &QAction::triggered, this, [this]() { setTheme(0); });
    connect(m_darkThemeAct, &QAction::triggered, this, [this]() { setTheme(1); });
    // 启动时根据持久化的主题勾选（唯一选中项）
    m_lightThemeAct->setChecked(m_theme == 0);
    m_darkThemeAct->setChecked(m_theme == 1);

    // 工具栏
    QHBoxLayout* toolBar = new QHBoxLayout();
    toolBar->setSpacing(8);
    toolBar->setContentsMargins(0, 4, 0, 4);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(getlang(0x22).toString());
    m_searchEdit->setMinimumWidth(220);
    m_searchEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &UninstallerWindow::filterSoftware);

    // Ctrl+F 快速聚焦搜索框
    QShortcut* searchShortcut = new QShortcut(QKeySequence("Ctrl+F"), this);
    connect(searchShortcut, &QShortcut::activated, this, [this]() {
        m_searchEdit->setFocus();
        m_searchEdit->selectAll();
    });

    m_refreshBtn = new QPushButton(getlang(0x23).toString(), this);
    m_refreshBtn->setObjectName("refreshBtn");
    connect(m_refreshBtn, &QPushButton::clicked, this, [this]() {
        built_list();       // 重新扫描注册表（反映新装/卸载的软件）
        loadSoftwareList(); // 重新加载并套用当前搜索过滤
    });

    m_scanLabel = new QLabel(getlang(0x24).toString());
    toolBar->addWidget(m_scanLabel);
    // 搜索框随窗口宽度自动伸缩，占满中间空白
    toolBar->addWidget(m_searchEdit, /*stretch=*/1);
    toolBar->addWidget(m_refreshBtn);

    // 仅显示残留项过滤
    m_orphanOnlyCheck = new QCheckBox(QString::fromUtf8(u8"仅显示残留项"), this);
    connect(m_orphanOnlyCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_showOrphanOnly = on;
        filterSoftware();
    });
    toolBar->addWidget(m_orphanOnlyCheck);

    // 语言下拉框：放在工具栏最右，比菜单栏更显眼，点击即时切换。
    m_langCombo = new QComboBox(this);
    for (int i = 0; i < langCount(); ++i) m_langCombo->addItem(langName(i, G.LANGUAGE));
    m_langCombo->setCurrentIndex(G.LANGUAGE);
    m_langCombo->setToolTip(getlang(0x3D).toString());
    connect(m_langCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) { setLanguage(idx); });
    toolBar->addWidget(m_langCombo);

    mainLayout->addLayout(toolBar);

    // 软件列表
    m_tableWidget = new QTableWidget(this);
    m_tableWidget->setColumnCount(7);
    QStringList headers = { getlang(0x25).toString(), getlang(0x26).toString(), getlang(0x27).toString(), getlang(0x28).toString(), getlang(0x29).toString(), getlang(0x2A).toString(), getlang(0x42).toString() };
    m_tableWidget->setHorizontalHeaderLabels(headers);
    m_tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tableWidget->setSelectionMode(QAbstractItemView::ExtendedSelection); // ③ 支持 Ctrl/Shift 多选
    m_tableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tableWidget->setAlternatingRowColors(true);
    m_tableWidget->setShowGrid(true);
    m_tableWidget->verticalHeader()->setDefaultSectionSize(34);
    m_tableWidget->verticalHeader()->setVisible(false);
    m_tableWidget->horizontalHeader()->setStretchLastSection(true);
    m_tableWidget->horizontalHeader()->setSectionsClickable(true);
    m_tableWidget->horizontalHeader()->setSortIndicatorShown(true);
    m_tableWidget->setSortingEnabled(true);

    // 右键菜单：卸载 / 扫描残留 / 详情 / 复制命令
    // 用事件过滤器直接在 viewport 上拦截右键，比 contextMenuPolicy 更可靠。
    m_tableWidget->viewport()->installEventFilter(this);

    mainLayout->addWidget(m_tableWidget);

    // 按钮栏
    QHBoxLayout* buttonBar = new QHBoxLayout();

    m_batchUninstallBtn = new QPushButton(QString::fromUtf8(u8"批量卸载"), this);
    m_batchUninstallBtn->setObjectName("batchUninstallBtn");
    connect(m_batchUninstallBtn, &QPushButton::clicked, this, &UninstallerWindow::batchUninstall);
    m_batchDelBtn = new QPushButton(QString::fromUtf8(u8"批量删残留"), this);
    m_batchDelBtn->setObjectName("batchDelBtn");
    connect(m_batchDelBtn, &QPushButton::clicked, this, &UninstallerWindow::batchDeleteResiduals);

    m_uninstallBtn = new QPushButton(getlang(0x1f).toString(), this);
    m_uninstallBtn->setObjectName("uninstallBtn");
    connect(m_uninstallBtn, &QPushButton::clicked, this, &UninstallerWindow::uninstallSelected);

    m_scanBtn = new QPushButton(getlang(0x20).toString(), this);
    m_scanBtn->setObjectName("scanBtn");
    connect(m_scanBtn, &QPushButton::clicked, this, &UninstallerWindow::scanResiduals);

    m_detailsBtn = new QPushButton(getlang(0x21).toString(), this);
    m_detailsBtn->setObjectName("detailsBtn");
    connect(m_detailsBtn, &QPushButton::clicked, this, &UninstallerWindow::showDetails);

    buttonBar->addWidget(m_batchUninstallBtn);
    buttonBar->addWidget(m_batchDelBtn);
    buttonBar->addWidget(m_uninstallBtn);
    buttonBar->addWidget(m_scanBtn);
    buttonBar->addWidget(m_detailsBtn);
    buttonBar->addStretch();

    mainLayout->addLayout(buttonBar);

    retranslateUI();   // 集中套用一次静态文本，保证后续切语言时只需刷新此处
}

#include "mainwindow.moc"
