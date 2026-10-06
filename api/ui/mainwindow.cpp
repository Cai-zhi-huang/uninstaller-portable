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
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDesktopServices>
#include <QUrl>
#include <QTimer>
#include <algorithm>

// 版本检查地址：绑定极空间 cpolar 隧道（http://192.168.31.128:8888 经 cpolar 暴露的公网地址）。
// 更新检查地址：指向 GitHub Pages 上的 version.json（稳定，不随 NAS/cpolar 部署变化）。
// 每次启动自动异步拉取（见 checkForUpdate），发现新版本会在更新弹窗追加提示横幅 + 前往下载按钮。
// 注意：软件运行在用户本机（非校园网），访问 github.io 正常；博客给同学看的穿透走 NAS cpolar，与此无关。
static const char* const kUpdateCheckUrl = "https://cai-zhi-huang.github.io/version.json";
#include <QMenu>
#include <QEvent>
#include <QContextMenuEvent>
#include <QProcess>
#include <QFormLayout>
#include <QGridLayout>
#include <QPainter>
#include <QImage>
#include <QMouseEvent>
#include <QFontMetrics>
#include <qt_windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <srrestoreptapi.h>
#include <QThread>
#include <QVector>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStyledItemDelegate>

// 名称列搜索高亮委托：在基类绘制（图标+文字+选中背景）之上，叠加高亮搜索命中片段。
// 不继承 Q_OBJECT，避免 .cpp 内 moc 负担；定义在首次使用（filterSoftware）之前。
class HighlightDelegate : public QStyledItemDelegate {
public:
    explicit HighlightDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}
    void setTokens(const QStringList& t) { m_tokens = t; }
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        if (m_tokens.isEmpty()) return;
        QString text = index.data(Qt::DisplayRole).toString();
        if (text.isEmpty()) return;
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
        QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget);
        QFontMetrics fm(painter->fontMetrics());
        const bool selected = (opt.state & QStyle::State_Selected);
        QColor bg(255, 214, 0, 90);
        QColor fg = selected ? QColor(255, 255, 255) : QColor(0xB0, 0x60, 0x00);
        painter->save();
        for (const QString& tk : m_tokens) {
            if (tk.isEmpty()) continue;
            int from = 0;
            while (true) {
                int pos = text.indexOf(tk, from, Qt::CaseInsensitive);
                if (pos < 0) break;
                int lead = fm.horizontalAdvance(text.left(pos));
                int w = fm.horizontalAdvance(text.mid(pos, tk.length()));
                QRect r(textRect.left() + lead, textRect.top(), w, textRect.height());
                painter->fillRect(r, bg);
                painter->setPen(fg);
                painter->drawText(r, Qt::AlignLeft | Qt::AlignVCenter, text.mid(pos, tk.length()));
                from = pos + tk.length();
            }
        }
        painter->restore();
    }
private:
    QStringList m_tokens;
};

// 把一行诊断信息追加到 exe 同级的 startup.log；带体积上限，超过则仅保留尾部，
// 防止每次启动追加导致日志无限增长（诊断日志失控占用/泄露历史）。
// 同时供 main.cpp 自检日志复用，避免重复实现。
void appendStartupLog(const QString& line) {
    const QString logPath = QCoreApplication::applicationDirPath() + QStringLiteral("/startup.log");
    constexpr qint64 kMaxStartLog = 64 * 1024; // 64 KB 上限
    QFile f(logPath);
    if (f.exists() && f.size() > kMaxStartLog) {
        // 截断：保留最近一半内容，再追加新行
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QByteArray tail = f.readAll().right(int(kMaxStartLog / 2));
            f.close();
            QFile w(logPath);
            if (w.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
                w.write(tail);
        }
    }
    if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts << line << '\n';
    }
}

// 结构化安全事件日志：每行一条 JSON（ts/rule/event/detail），供威胁检测系统（FIM 脚本、Sysmon 配套）采集。
// 与检测方案 R3/R5/R6 对应；带 1MB 上限，截断时保留最近一半内容。
void logSecurityEvent(const QString& rule, const QString& eventType, const QString& detail) {
    const QString logPath = QCoreApplication::applicationDirPath() + QStringLiteral("/detection.log");
    constexpr qint64 kMaxDetectLog = 1024 * 1024; // 1 MB 上限
    QFile f(logPath);
    if (f.exists() && f.size() > kMaxDetectLog) {
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QByteArray tail = f.readAll().right(int(kMaxDetectLog / 2));
            f.close();
            QFile w(logPath);
            if (w.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
                w.write(tail);
        }
    }
    QJsonObject o;
    o[QStringLiteral("ts")]     = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    o[QStringLiteral("rule")]   = rule;
    o[QStringLiteral("event")]  = eventType;
    o[QStringLiteral("detail")] = detail;
    const QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact);
    QFile out(logPath);
    if (out.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&out);
        ts << line << '\n';
    }
}
#include <QJsonArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QCoreApplication>
#include <QtConcurrent>
#include <QFutureWatcher>
#include "systools.hpp"

// ================= 操作历史日志 =================
// 每次卸载/删残留/强制删除都追加一条 JSONL 记录（ts/op/name/success/detail），
// 供「查看操作历史」对话框回溯与导出。带 2MB 上限，截断保留最近一半。
QString UninstallerWindow::opLogPath() const {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
           + QStringLiteral("/op_history.log");
}

void UninstallerWindow::logOperation(const QString& op, const QString& name,
                                     bool success, const QString& detail) {
    const QString path = opLogPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    constexpr qint64 kMax = 2 * 1024 * 1024;
    QFile f(path);
    if (f.exists() && f.size() > kMax) {
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QByteArray tail = f.readAll().right(int(kMax / 2));
            f.close();
            QFile w(path);
            if (w.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
                w.write(tail);
        }
    }
    QJsonObject o;
    o[QStringLiteral("ts")]      = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    o[QStringLiteral("op")]      = op;
    o[QStringLiteral("name")]    = name;
    o[QStringLiteral("success")] = success;
    o[QStringLiteral("detail")]  = detail;
    QFile out(path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&out);
        ts << QJsonDocument(o).toJson(QJsonDocument::Compact) << '\n';
    }
}

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
// 图标链路诊断：exe 同级存在 icon_debug.on 标记文件时把提取过程逐环追加到
// 同级 startup.log，用于排查「列表图标全是默认图标」一类问题；
// 删除标记文件即关闭，正常使用零额外磁盘写入。
static void logIconStep(const QString& line) {
    static const bool enabled = QFileInfo::exists(
        QCoreApplication::applicationDirPath() + QStringLiteral("/icon_debug.on"));
    if (!enabled) return;
    appendStartupLog(QStringLiteral("[icon] ") + line);
}

// 图标提取结果缓存：同一文件+索引在一次会话内只抽一次。
// 列表过滤/刷新会频繁重建行并重新提取图标，缓存避免 GUI 线程重复 GDI 调用导致卡顿，
// 重扫列表也能秒开（~242 个软件不再每次都 ExtractIconExW）。
static QMutex g_iconCacheMutex;
static QHash<QString, QImage> g_iconCache;

// 核心提取逻辑（不含缓存）。
static QImage extractIconImageImpl(const QString& filePath, int index) {
    if (filePath.isEmpty()) return QImage();

    QString lower = filePath.toLower();
    if (lower.endsWith(".ico") || lower.endsWith(".png") ||
        lower.endsWith(".bmp") || lower.endsWith(".jpg") ||
        lower.endsWith(".jpeg") || lower.endsWith(".webp")) {
        // 独立图片文件：直接用 QImage 加载（QImage 线程安全）。
        QImage img(filePath);
        if (!img.isNull() && imageHasVisiblePixels(img)) return img;
        // 加载失败不再直接放弃：部分软件（如 Adobe）把 PE 文件伪装成 .ico 写进
        // ProductIcon，QImage 读不了但 ExtractIconExW 能抽；落到下方可执行分支再试。
        logIconStep(QStringLiteral("img-load miss '%1'").arg(filePath));
    }

    // 无扩展名 / 非可执行扩展名的文件（如 MSI ProductIcon 常见的
    // C:\Windows\Installer\{GUID}\ProductIcon 无扩展名 ico）：QImage 按内容嗅探加载。
    // 限 8MB 防止把大文件当图片读；exe/dll 等可执行格式交给 ExtractIconExW。
    QFileInfo fiChk(filePath);
    if (fiChk.exists()) {
        QString suf = fiChk.suffix().toLower();
        static const QStringList kPeExt = {
            "exe", "dll", "ocx", "cpl", "scr", "icl", "mui", "drv", "sys", "ime", "fon"};
        if (!kPeExt.contains(suf) && fiChk.size() <= 8 * 1024 * 1024) {
            QImage img(filePath);
            if (!img.isNull() && imageHasVisiblePixels(img)) return img;
        }
    }

    // 可执行文件：锁定/只读卷直接跳过，避免 ExtractIconExW 阻塞。
    if (informat::volumeAccessState(filePath.toStdString()) != 0) {
        return QImage();
    }

    std::wstring wPath = QDir::toNativeSeparators(filePath).toStdWString();
    // 优先 SHDefExtractIconW 按请求尺寸抽取“最接近请求尺寸的最佳图标资源”。
    // 现代应用多含 256px PNG 图标，按 64px 抽出后由 Qt 平滑缩放到列表 20px / HiDPI，
    // 比 ExtractIconExW 固定返回 32px 大图标更清晰锐利。
    HICON hIcon = nullptr;
    if (SUCCEEDED(SHDefExtractIconW(wPath.c_str(), index, 0, &hIcon, nullptr, 64)) && hIcon) {
        QImage img = imageFromHICON(hIcon);
        DestroyIcon(hIcon);
        if (imageHasVisiblePixels(img)) return img;
    }
    // 兜底：ExtractIconExW 大/小图标（部分图标资源索引只存了小图标，或老系统无 SHDefExtractIconW）。
    HICON hIconLarge = nullptr;
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

static QImage extractIconImage(const QString& filePath, int index = 0) {
    if (filePath.isEmpty()) return QImage();
    const QString key = filePath + QLatin1Char('|') + QString::number(index);
    {
        QMutexLocker lock(&g_iconCacheMutex);
        auto it = g_iconCache.constFind(key);
        if (it != g_iconCache.constEnd()) return it.value();
    }
    QImage img = extractIconImageImpl(filePath, index);
    {
        QMutexLocker lock(&g_iconCacheMutex);
        g_iconCache.insert(key, img);
    }
    return img;
}

static QIcon iconFromFile(const QString& filePath, int index = 0) {
    QImage img = extractIconImage(filePath, index);
    if (!img.isNull()) {
        return QIcon(QPixmap::fromImage(img));
    }
    return QIcon();
}

// 从 SoftwareInfo::displayIcon 提取图标；缺失或失败时再尝试安装目录 exe、
// 卸载命令 exe，最后回退到确定可见的默认应用图标，避免列表出现空白图标。
static QString extractExePath(const QString& cmd); // 前向声明（定义在文件下方）

// MSI 产品图标兜底：读 HKLM\SOFTWARE\Classes\Installer\Products\<压缩GUID>\ProductIcon。
// 很多 MSI 安装（Adobe/NVIDIA/Java 等）的 Uninstall 键没有 DisplayIcon，
// 只有 Installer\Products 里有 ProductIcon（可能是 .ico、无扩展名 ico 或指向 exe）。
// 把标准 GUID 转成 Windows Installer 的压缩格式：前 3 段逐字符反转，后 2 段每 2 字符交换。
static QString msiProductIconPath(const SoftwareInfo* sw) {
    if (!sw || sw->regPath.empty()) return QString();
    size_t slash = sw->regPath.find_last_of("\\/");
    std::string leaf = (slash == std::string::npos) ? sw->regPath : sw->regPath.substr(slash + 1);
    if (leaf.size() < 2 || leaf.front() != '{' || leaf.back() != '}') return QString();
    std::string g;
    for (char c : leaf.substr(1, leaf.size() - 2))
        if (c != '-') g += c;   // 去掉连字符：regPath 叶子是带「-」的 GUID，压缩格式要 32 位纯 hex
    if (g.size() != 32) return QString();
    std::string sq;
    for (size_t i = 8; i-- > 0;)      sq += g[i];          // 段1 反转
    for (size_t i = 12; i-- > 8;)     sq += g[i];          // 段2 反转
    for (size_t i = 16; i-- > 12;)    sq += g[i];          // 段3 反转
    for (size_t i = 16; i < 32; i += 2) { sq += g[i + 1]; sq += g[i]; } // 段4/5 两两交换
    std::string val = Registry::readString(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Classes\\Installer\\Products\\" + sq, "ProductIcon");
    logIconStep(QStringLiteral("msi regPath='%1' sq='%2' raw='%3'")
                    .arg(QString::fromStdString(sw->regPath),
                         QString::fromStdString(sq),
                         QString::fromStdString(val)));
    if (val.empty()) {
        // 按用户安装的 MSI（如 Speedy）不走 HKLM，advertise 图标在
        // %APPDATA%\Microsoft\Installer\{GUID}\ProductIcon（常为无扩展名 ico 或 exe 副本）。
        QByteArray appData = qgetenv("APPDATA");
        if (!appData.isEmpty()) {
            QString perUser = QString::fromLocal8Bit(appData)
                + "\\Microsoft\\Installer\\" + QString::fromStdString(leaf)
                + "\\ProductIcon";
            if (QFileInfo::exists(perUser)) {
                logIconStep(QStringLiteral("msi per-user fallback '%1'").arg(perUser));
                return perUser;
            }
        }
        logIconStep(QStringLiteral("msi miss regPath='%1'").arg(QString::fromStdString(sw->regPath)));
        return QString();
    }
    // 部分 MSI 把 ProductIcon 写成 D:\\bin\\x.exe 这类重复反斜杠，统一规整
    QString p = QString::fromStdString(val);
    p.replace("\\\\", "\\");
    return p.trimmed();
}

// 从 SoftwareInfo::displayIcon（如 "C:\app.exe" 或 "C:\app.dll,-123"）提取图标。
// DisplayIcon 经常被加上双引号，提取前先去掉，否则 ExtractIconExW 会失败。

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
        logIconStep(QStringLiteral("step1 '%1' displayIcon='%2' -> %3")
                        .arg(displayName, path, img.isNull() ? "miss" : "HIT"));
        if (!img.isNull()) return img;
    }

    // 1b) MSI 产品图标兜底：Uninstall 键无 DisplayIcon 时（Adobe/NVIDIA/Java 等
    //     普遍如此），从 Installer\Products 的 ProductIcon 取（支持 .ico 与无扩展名 ico）。
    if (!sw->regPath.empty()) {
        QString pi = msiProductIconPath(sw);
        if (!pi.isEmpty()) {
            QString p = pi;
            int ci = p.lastIndexOf(',');
            int idx = 0;
            if (ci != -1) {
                bool ok = false;
                int ii = p.mid(ci + 1).toInt(&ok);
                if (ok) { idx = ii; p = p.left(ci); }
            }
            p = stripQuotes(p).trimmed();
            QImage img = extractIconImage(p, idx);
            logIconStep(QStringLiteral("msi 1b name='%1' pi='%2' -> imgNull=%3")
                            .arg(QString::fromStdString(sw->displayName), p,
                                 img.isNull() ? "Y" : "N"));
            if (!img.isNull()) return img;
        }
    }

    // 2) 安装目录里找 exe：完整模式 DFS ≤2 层；fastMode 只扫根层（不递归，
    //    开销可控）——否则 DisplayIcon 失效的条目（如微信旧版指向已删路径）
    //    在初始列表里永远只能拿到默认图标。
    {
        QString installLoc = stripQuotes(QString::fromStdString(sw->installLocation)).trimmed();
        QStringList cands;
        if (fastMode) {
            QDir rootDir(installLoc);
            if (!installLoc.isEmpty() && rootDir.exists()) {
                const QFileInfoList fis = rootDir.entryInfoList(QStringList() << "*.exe",
                                                                QDir::Files | QDir::NoDotAndDotDot,
                                                                QDir::Name);
                for (const QFileInfo& fi : fis) cands.append(fi.absoluteFilePath());
            }
        } else {
            cands = collectInstallExes(installLoc, displayName,
                                       QString::fromStdString(sw->publisher).trimmed());
        }
        if (!cands.isEmpty())
            logIconStep(QStringLiteral("step2 '%1' installLoc='%2' 候选=%3 mode=%4")
                            .arg(displayName, installLoc, QString::number(cands.size()),
                                 fastMode ? "fast" : "full"));
        // 辅助程序（卸载器/更新器等）不能代表软件图标，先剔除；再把与软件名
        // 匹配的 exe 排到最前（如 Weixin 目录中 Weixin.exe 优先于 Uninstall.exe，
        // 否则按字母序 Uninstall 会抢先命中，列表出现卸载器的图标）。
        QStringList ranked;
        for (const QString& c : cands) {
            const QString bn = QFileInfo(c).baseName();
            // 用 "unins" 一把覆盖 uninstall / uninstaller / unins000(Inno Setup) /
            // uninst(NSIS) 等卸载器命名，避免列表显示卸载器齿轮图标冒充应用图标。
            if (bn.contains("unins", Qt::CaseInsensitive) ||
                bn.contains("update", Qt::CaseInsensitive) ||
                bn.contains("setup", Qt::CaseInsensitive) ||
                bn.contains("crash", Qt::CaseInsensitive) ||
                bn.contains("report", Qt::CaseInsensitive) ||
                bn.contains("repair", Qt::CaseInsensitive)) {
                logIconStep(QStringLiteral("step2 '%1' 跳过辅助程序 '%2'")
                                .arg(displayName, c));
                continue;
            }
            ranked.append(c);
        }
        std::stable_sort(ranked.begin(), ranked.end(),
                         [&displayName](const QString& a, const QString& b) {
                             return QFileInfo(a).baseName().contains(displayName, Qt::CaseInsensitive)
                                  && !QFileInfo(b).baseName().contains(displayName, Qt::CaseInsensitive);
                         });
        for (const QString& exePath : ranked) {
            QImage img = extractIconImage(exePath, 0);
            logIconStep(QStringLiteral("step2 '%1' 尝试 '%2' -> %3")
                            .arg(displayName, exePath, img.isNull() ? "miss" : "HIT"));
            if (!img.isNull()) return img;
        }
    }

    // 3) 卸载命令里解析出的 exe（最后兜底）。卸载器/msiexec 不是应用图标，
    // 跳过以免列表出现“卸载器齿轮图标”或“Windows Installer 图标”冒充应用图标；
    // 跳过后回退到默认彩色占位或进程扫描（GUI 完整版 step4）。
    QString cmd = QString::fromStdString(Registry::getUninstallCommand(*sw));
    QString exe = extractExePath(cmd);
    if (!exe.isEmpty()) {
        const QString ebn = QFileInfo(exe).baseName().toLower();
        if (ebn.contains("unins") || ebn == "msiexec") {
            logIconStep(QStringLiteral("step3 '%1' 跳过卸载器 '%2'")
                            .arg(displayName, exe));
        } else if (QFile::exists(exe)) {
            QImage img = extractIconImage(exe, 0);
            logIconStep(QStringLiteral("step3 '%1' '%2' -> %3")
                            .arg(displayName, exe, img.isNull() ? "miss" : "HIT"));
            if (!img.isNull()) return img;
        } else {
            logIconStep(QStringLiteral("step3 '%1' '%2' 文件不存在").arg(displayName, exe));
        }
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
        logIconStep(QStringLiteral("step4 '%1' 窗口匹配='%2' 存在=%3")
                        .arg(displayName, runningExe,
                             QFile::exists(runningExe) ? "Y" : "N"));
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
                    // 过滤更新/卸载/崩溃报告等辅助程序（如腾讯
                    // C:\ProgramData\Tencent\WeChat\WeChatUpdate.exe 会给出错误的绿方块图标），
                    // 只取主程序候选，避免列表显示错误的图标。
                    QFileInfoList ranked;
                    for (const QFileInfo& fi : exes) {
                        const QString bn = fi.baseName().toLower();
                        if (bn.contains("update") || bn.contains("unins") ||
                            bn.contains("setup") || bn.contains("crash") ||
                            bn.contains("report") || bn.contains("repair") ||
                            bn.contains("helper") || bn.contains("launcher")) {
                            logIconStep(QStringLiteral("step5 '%1' 跳过辅助程序 '%2'")
                                            .arg(displayName, fi.absoluteFilePath()));
                            continue;
                        }
                        ranked.append(fi);
                    }
                    logIconStep(QStringLiteral("step5 '%1' dir='%2' exe总数=%3 过滤后=%4")
                                    .arg(displayName, base, QString::number(exes.size()),
                                         QString::number(ranked.size())));
                    for (const QFileInfo& fi : ranked) {
                        QImage img = extractIconImage(fi.absoluteFilePath(), 0);
                        logIconStep(QStringLiteral("step5 '%1' 尝试 '%2' -> %3")
                                        .arg(displayName, fi.absoluteFilePath(),
                                             img.isNull() ? "miss" : "HIT"));
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
        padding: 4px 6px;
        color: #e0e3e8;
        background-color: transparent;
    }
    QTableWidget::item:hover {
        background-color: #2c323b;
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
    QScrollBar:vertical { background-color: #202328; width: 12px; border-radius: 6px; }
    QScrollBar::handle:vertical { background-color: #3c424d; border-radius: 6px; min-height: 30px; }
    QScrollBar::handle:vertical:hover { background-color: #4a525e; }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { background: none; }
    QScrollBar:horizontal { background-color: #202328; height: 12px; border-radius: 6px; }
    QScrollBar::handle:horizontal { background-color: #3c424d; border-radius: 6px; min-width: 30px; }
    QScrollBar::handle:horizontal:hover { background-color: #4a525e; }
    QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { background: none; }

    QComboBox { background-color: #202328; color: #e0e3e8; border: 1px solid #3c424d; border-radius: 6px; padding: 4px 8px; }
    QComboBox:hover { border: 1px solid #5c9ad6; }
    QComboBox QAbstractItemView { background-color: #202328; color: #e0e3e8; selection-background-color: #353a43; border: 1px solid #3c424d; }

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
        padding: 4px 6px;
        color: #1a1a1a;
        background-color: transparent;
    }
    QTableWidget::item:hover {
        background-color: #eef3f8;
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
    QScrollBar:vertical { background-color: #f2f2f2; width: 12px; border-radius: 6px; }
    QScrollBar::handle:vertical { background-color: #c0c0c0; border-radius: 6px; min-height: 30px; }
    QScrollBar::handle:vertical:hover { background-color: #a8a8a8; }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { background: none; }
    QScrollBar:horizontal { background-color: #f2f2f2; height: 12px; border-radius: 6px; }
    QScrollBar::handle:horizontal { background-color: #c0c0c0; border-radius: 6px; min-width: 30px; }
    QScrollBar::handle:horizontal:hover { background-color: #a8a8a8; }
    QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { background: none; }

    QComboBox { background-color: #ffffff; color: #1a1a1a; border: 1px solid #b0b0b0; border-radius: 6px; padding: 4px 8px; }
    QComboBox:hover { border: 1px solid #4a90d9; }
    QComboBox QAbstractItemView { background-color: #ffffff; color: #1a1a1a; selection-background-color: #e6e6e6; border: 1px solid #c0c0c0; }

    QMessageBox { background-color: #ffffff; }
)";


UninstallerWindow::UninstallerWindow(QWidget* parent) : QMainWindow(parent) {
    // 版本检查网络管理器：随窗口生命周期存活，finished 统一路由到回调
    m_netMgr = new QNetworkAccessManager(this);
    connect(m_netMgr, &QNetworkAccessManager::finished,
            this, &UninstallerWindow::onVersionReplyFinished);
}

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
    // ⑨ 恢复持久化的主题；首次启动则跟随系统明暗主题
    {
        QSettings st(QStringLiteral("Uninstaller"), QStringLiteral("uninstaller"));
        if (st.contains(QStringLiteral("theme"))) {
            m_theme = st.value(QStringLiteral("theme"), 0).toInt();
        } else {
            // 首次启动：读取 Windows 个性化设置（AppsUseLightTheme=0 为深色）
            QSettings sys(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
                          QSettings::NativeFormat);
            bool light = sys.value(QStringLiteral("AppsUseLightTheme"), 1).toInt() != 0;
            m_theme = light ? 0 : 1;
            st.setValue(QStringLiteral("theme"), m_theme);  // 记录一次，后续以用户手动切换为准
        }
        setTheme(m_theme);
    }
    loadCustomBgSettings();  // 恢复自定义背景（需在首次 setTheme 后补套透明化样式）
    if (!m_bgPixmap.isNull()) setTheme(m_theme);
    setupUI();
    built_list(/*allowCache=*/true);   // 启动：命中1小时内缓存则跳过扫描与进度条
    loadSoftwareList();
    // ⑩ 恢复上次保存的窗口几何与列表列宽（首次启动无记录则用默认）
    {
        QSettings geo(QStringLiteral("Uninstaller"), QStringLiteral("uninstaller"));
        restoreGeometry(geo.value(QStringLiteral("windowGeometry")).toByteArray());
        if (m_tableWidget && m_tableWidget->horizontalHeader()) {
            const QByteArray hs = geo.value(QStringLiteral("tableHeaderState")).toByteArray();
            if (!hs.isEmpty()) m_tableWidget->horizontalHeader()->restoreState(hs);
        }
    }
    show();
    checkForUpdate();    // 启动即异步拉取最新版本（不阻塞界面，结果写回更新弹窗）
    showUpdatePopup();   // 启动即弹出更新日志
}

void UninstallerWindow::fresh() {
    setupUI();
    show();
}

void UninstallerWindow::built_list(bool allowCache) {
    // 启动时使用缓存：若1小时内有缓存则直接读取，跳过注册表扫描与进度条
    if (allowCache && loadSoftwareCache()) {
        // 旧缓存可能产生于「大小去重」修复之前（NVIDIA 等同目录条目重复计费），
        // 幂等地再跑一次去重并重算总大小；新缓存再跑结果不变。
        Registry::dedupeSharedLocationSizes(m_softwareList);
        total_size = filesize_t();
        for (auto& s : m_softwareList) total_size += s.size.size;
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
    int lastProgress = 0;
    connect(&watcher, &QFutureWatcher<void>::finished, &loop, &QEventLoop::quit);
    connect(&watcher, &QFutureWatcher<void>::progressValueChanged,
            &progress, [&](int v) {
                lastProgress = v;
                progress.setValue(v);
                QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            });
    // 「取消」按钮/Esc：置位扫描取消标志，findExeOnDisk 的磁盘遍历在检查点快速退出，
    // QtConcurrent::map 随即收尾。取消后的列表不完整，不写缓存（下次启动重新扫描）。
    // 注意：进度到达 100% 时 QProgressDialog 会 autoReset 复位，此时可能伴随发出
    // canceled（Qt 6.10 实测）；扫描已完成，这个“取消”必须忽略，否则会误判为
    // 用户取消而白白丢弃完整扫描结果（不写缓存）。lastProgress>=len 即已完成。
    bool scanAborted = false;
    connect(&progress, &QProgressDialog::canceled, &progress, [&]() {
        if (lastProgress >= len) {
            appendStartupLog(QStringLiteral("[scan] canceled ignored (already 100%)"));
            return;
        }
        scanAborted = true;
        appendStartupLog(QStringLiteral("[scan] cancel clicked at %1/%2, aborting").arg(lastProgress).arg(len));
        Registry::requestScanAbort();
    });
    Registry::resetScanAbort();
    // 并行扫描前只枚举一次进程名快照，所有 registryInit 任务只读复用，
    // 避免几百个软件在后台线程各自 CreateToolhelp32Snapshot 一次（性能优化，行为不变）。
    std::vector<std::wstring> runningProcs = Registry::snapshotRunningProcesses();
    watcher.setFuture(QtConcurrent::map(m_softwareList,
        [runningProcs](SoftwareInfo& sw) { sw.registryInit(runningProcs); }));
    loop.exec();

    // 大小去重必须在 registryInit 全部完成之后：registryInit 对 EstimatedSize==0
    // 的条目按 InstallLocation 重扫目录，会覆盖扫描前去重清零的值。NVIDIA CUDA
    // 的几十个子组件共享同一安装目录，不去重会让总大小虚高数十 GB。
    Registry::dedupeSharedLocationSizes(m_softwareList);

    for (auto& sw : m_softwareList) total_size += sw.size.size;
    progress.close();

    // 实时扫描完成后写缓存（供1小时内再次启动时免加载条）；被取消的扫描不完整，不写
    appendStartupLog(QStringLiteral("[scan] finished, aborted=%1 items=%2")
                         .arg(scanAborted).arg(m_softwareList.size()));
    if (!scanAborted) saveSoftwareCache();
}

// 缓存完整性盐：与缓存内容一起参与哈希，使“直接清空 items 数组”等粗陋投毒失效。
// 说明：这是防君子不防小人的轻量护栏——能写 exe 目录的攻击者也能从二进制中提取此盐并重算校验值；
// 但它能挡住最常见的“随手改 JSON 隐藏已装软件”，并能在缓存意外损坏时回退到实时扫描。
static const QByteArray kCacheIntegritySalt = QByteArrayLiteral(
    "UninstallerMgr-cache-v1-9f3c2a7b-d4e8-4b1c-8a6f-2e5d9c0b1a3f");

static QByteArray cacheIntegrityOf(const QJsonArray& items) {
    const QByteArray payload = QJsonDocument(items).toJson(QJsonDocument::Compact);
    return QCryptographicHash::hash(payload + kCacheIntegritySalt, QCryptographicHash::Sha256).toHex();
}

bool UninstallerWindow::loadSoftwareCache() {
    const QString path = QCoreApplication::applicationDirPath() + QStringLiteral("/uninstaller_cache.json");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    // 缓存体积护栏：超过 50 MB 视为异常/被投毒，丢弃回退实时扫描（避免 readAll 消耗过多内存）
    if (f.size() > qint64(50 * 1024 * 1024)) return false;
    const QByteArray raw = f.readAll();
    QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (doc.isNull() || !doc.isObject()) return false;
    QJsonObject root = doc.object();
    const qint64 ts = root.value(QStringLiteral("timestamp")).toVariant().toLongLong();
    // 仅在1小时内有效（3600 * 1000 ms）
    if (QDateTime::currentMSecsSinceEpoch() - ts > 3600LL * 1000LL) return false;
    const QJsonArray items = root.value(QStringLiteral("items")).toArray();
    if (items.isEmpty()) return false;

    // 完整性校验（F8）：缺失或不符均视为被篡改/损坏，丢弃缓存、回退实时扫描。
    const QByteArray expect = cacheIntegrityOf(items);
    if (root.value(QStringLiteral("integrity")).toString().toLatin1() != expect) {
        // R3（缓存校验失败）：完整性不符视为被篡改/损坏，记录结构化安全事件后回退实时扫描。
        logSecurityEvent(QStringLiteral("R3"),
                         QStringLiteral("cache_integrity_fail"),
                         QStringLiteral("uninstaller_cache.json 完整性校验失败，疑似被篡改或损坏，已回退实时扫描"));
        return false;
    }

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
    // 完整性校验（F8）：把 items 的哈希随缓存一起写出，加载时校验，挡住粗陋投毒。
    root[QStringLiteral("integrity")] = QString::fromLatin1(cacheIntegrityOf(items));
    QJsonDocument doc(root);
    const QString path = QCoreApplication::applicationDirPath() + QStringLiteral("/uninstaller_cache.json");
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const qint64 written = f.write(doc.toJson(QJsonDocument::Compact));
        f.close();
        appendStartupLog(QStringLiteral("[cache] saved %1 bytes").arg(written));
    } else {
        appendStartupLog(QStringLiteral("[cache] OPEN FAILED err=%1 path=%2")
                             .arg(int(f.error())).arg(path));
    }
}

void UninstallerWindow::createTray() {
    m_tray = new QSystemTrayIcon(this);
    QIcon ic = QApplication::windowIcon();
    if (ic.isNull()) ic = QIcon(QStringLiteral(":/appicon.png"));
    if (ic.isNull()) ic = style()->standardIcon(QStyle::SP_ComputerIcon);
    m_tray->setIcon(ic);
    m_tray->setToolTip(windowTitle());

    m_trayMenu = new QMenu(this);
    QAction* showAct = m_trayMenu->addAction(QString::fromUtf8(u8"显示主界面"));
    connect(showAct, &QAction::triggered, this, [this]() {
        showNormal(); raise(); activateWindow();
    });
    m_trayMenu->addSeparator();
    QAction* quitAct = m_trayMenu->addAction(QString::fromUtf8(u8"退出"));
    connect(quitAct, &QAction::triggered, this, [this]() {
        m_forceQuit = true;
        close();   // 走完整退出路径（含线程清理与 ExitProcess）
    });
    m_tray->setContextMenu(m_trayMenu);
    connect(m_tray, &QSystemTrayIcon::activated, this, &UninstallerWindow::onTrayActivated);
    m_tray->setVisible(true);
}

void UninstallerWindow::onTrayActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger) {
        if (isHidden() || !isVisible()) {
            showNormal(); raise(); activateWindow();
        } else {
            // 已可见时左键单击收起到托盘（与双击恢复形成对称）
            hide();
        }
    }
}

void UninstallerWindow::closeEvent(QCloseEvent* event) {
    // 关闭前台窗口：若有托盘且非强制退出，则最小化到系统托盘（不结束进程）
    if (!m_forceQuit && m_tray && m_tray->isVisible()) {
        hide();
        event->ignore();
        static bool hinted = false;
        if (!hinted) {
            m_tray->showMessage(windowTitle(),
                                QString::fromUtf8(u8"已最小化到系统托盘，双击图标可恢复窗口。"),
                                QSystemTrayIcon::Information, 3000);
            hinted = true;
        }
        return;
    }

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

    // 退出前持久化窗口几何与列宽（供下次启动恢复）
    {
        QSettings geo(QStringLiteral("Uninstaller"), QStringLiteral("uninstaller"));
        geo.setValue(QStringLiteral("windowGeometry"), saveGeometry());
        if (m_tableWidget && m_tableWidget->horizontalHeader()) {
            geo.setValue(QStringLiteral("tableHeaderState"), m_tableWidget->horizontalHeader()->saveState());
        }
        // 关键：下方 ::ExitProcess(0) 不执行 C++ 栈展开/析构，QSettings 的延迟写入
        // 不会落盘，必须在此显式 sync()，否则窗口几何/表头状态每次退出都丢失。
        geo.sync();
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
        // 运行中软件不再独占一组（否则会被分组过滤 findlist 隐藏，从列表彻底消失）；
        // 空名称软件同样不再独占组 4（findlist 永远不含 4，会被整组隐藏、无法卸载），
        // 二者都按原有类别归入普通组 0 显示，名称列以占位符标出。
        if (sw->isSystemComponent)m_swlist[2].push_back(sw);
        else if (sw->isWindowsInstaller)m_swlist[1].push_back(sw);
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
            // 按设备像素比放大再缩放进 20px 槽位，HiDPI（125%/150%/200%）下不糊。
            const qreal dpr = (qApp && qApp->primaryScreen())
                                  ? qApp->primaryScreen()->devicePixelRatio()
                                  : 1.0;
            const int px = qMax(20, int(20 * dpr + 0.5));
            QPixmap pm = QPixmap::fromImage(img).scaled(
                px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation);
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
            // 空名称软件（DisplayName 缺失的注册表项）在名称列用占位符标出，
            // 保证其可见、可辨识、可选中卸载（先前因归入组 4 被过滤隐藏）。
            QString dispName = sw->displayName.empty()
                ? getlang(0x50).toString()
                : QString::fromStdString(sw->displayName);
            auto* nameItem = new QTableWidgetItem(dispName);
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
                auto* statusItem = new QTableWidgetItem(getlang(0x49).toString());
                QFont sf = statusItem->font();
                sf.setBold(true);
                statusItem->setFont(sf);
                statusItem->setForeground(QColor(0xE0, 0x5A, 0x5A));
                m_tableWidget->setItem(i, 6, statusItem);
                // 名称也染成警告色，方便一眼识别
                QFont nf = nameItem->font();
                nameItem->setForeground(QColor(0xD9, 0x7A, 0x7A));
                nameItem->setFont(nf);
            } else if (sw->isRunningTime) {
                // 运行中：蓝色加粗标注，方便用户识别当前正在运行的程序（不再因独占分组而消失）
                auto* statusItem = new QTableWidgetItem(getlang(0x4A).toString());
                QFont sf = statusItem->font();
                sf.setBold(true);
                statusItem->setFont(sf);
                statusItem->setForeground(QColor(0x4A, 0x90, 0xE2));
                m_tableWidget->setItem(i, 6, statusItem);
            } else if (isCriticalSystemItem(sw)) {
                // ⑦ 系统关键项：灰显并标注“不建议卸载”
                auto* statusItem = new QTableWidgetItem(getlang(0x4C).toString());
                statusItem->setForeground(QColor(0x9E, 0x9E, 0x9E));
                m_tableWidget->setItem(i, 6, statusItem);
                nameItem->setForeground(QColor(0x9E, 0x9E, 0x9E));
            } else {
                m_tableWidget->setItem(i, 6, new QTableWidgetItem(getlang(0x4B).toString()));
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
    // 仅首次填充时自动调整列宽（避免每次卸载/删除/刷新重扫都把用户拉宽的列重置回默认）。
    if (!m_columnsSized) {
        m_tableWidget->resizeColumnsToContents();
        m_columnsSized = true;
    }
    m_tableWidget->setSortingEnabled(true);
    // 保留用户此前设置的排序列/方向：重扫（卸载/删除/刷新）后不再强制回到
    // “名称升序”，否则用户刚按大小排好的列表会被重置，体验差。
    // 仅当从未手动排序过（sortIndicatorSection<0）时才回退默认名称升序。
    int sortCol = m_tableWidget->horizontalHeader()->sortIndicatorSection();
    Qt::SortOrder sortOrder = m_tableWidget->horizontalHeader()->sortIndicatorOrder();
    if (sortCol >= 0) {
        m_tableWidget->sortItems(sortCol, sortOrder);
    } else {
        m_tableWidget->sortItems(0, Qt::AscendingOrder);
    }
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

// 卸载前创建系统还原点（降低误卸载风险）。动态加载 SrClient.dll，不链接额外库；
// 非管理员/系统还原关闭时失败，静默跳过，绝不阻断卸载流程。
static bool createSystemRestorePoint(const QString& desc) {
    appendStartupLog(QStringLiteral("[restore] create for '%1'").arg(desc));
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = SUCCEEDED(hr);
    bool ok = false;
    if (HMODULE h = LoadLibraryW(L"SrClient.dll")) {
        using Pfn = BOOL (WINAPI*)(PRESTOREPOINTINFO, PSTATEMGRSTATUS);
        if (auto* pfn = reinterpret_cast<Pfn>(GetProcAddress(h, "SRSetRestorePointW"))) {
            RESTOREPOINTINFO info = {0};
            info.dwEventType = BEGIN_SYSTEM_CHANGE;
            info.dwRestorePtType = APPLICATION_INSTALL;
            const std::wstring d = L"UninstallerManager: " + desc.toStdWString();
            size_t n = d.size();
            if (n > _countof(info.szDescription) - 1) n = _countof(info.szDescription) - 1;
            wcsncpy(info.szDescription, d.c_str(), n);
            info.szDescription[n] = L'\0';
            STATEMGRSTATUS status = {0};
            ok = pfn(&info, &status) && status.nStatus == ERROR_SUCCESS;
            appendStartupLog(QStringLiteral("[restore] result=%1 nStatus=%2")
                                .arg(ok ? "ok" : "fail").arg(status.nStatus));
        } else {
            appendStartupLog(QStringLiteral("[restore] SRSetRestorePointW not found"));
        }
        FreeLibrary(h);
    } else {
        appendStartupLog(QStringLiteral("[restore] SrClient.dll load failed"));
    }
    if (needUninit) CoUninitialize();
    return ok;
}

// 执行单个卸载（不含确认/预览），单条与批量共用。返回是否成功。
UninstallResult UninstallerWindow::doUninstall(SoftwareInfo* software, bool showProgress) {
    if (!software) return UninstallResult::Failed;
    QString name = QString::fromStdString(software->displayName);
    // showProgress=false 时（批量卸载）不弹独立进度框，由调用方的总进度条统一反馈，
    // 避免每卸载一项就闪一个对话框、体验割裂。
    if (!showProgress) {
        return Registry::uninstallSoftware(*software);
    }
    // 单条卸载：卸载前先建系统还原点（失败静默跳过，不阻断卸载）
    createSystemRestorePoint(QString::fromStdString("卸载 " + software->displayName));
    QProgressDialog progress(getlang(0xCu).toString().arg(name),
        getlang(0x3u).toString(), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.show();
    QApplication::processEvents();
    UninstallResult result = Registry::uninstallSoftware(*software);
    progress.close();
    return result;
}

void UninstallerWindow::uninstallSelected() {
    int row = m_tableWidget->currentRow();
    if (tick(row))return ;

    auto software = softwareAtRow(row);
    if (!software) return;

    // ⑦-0 列表里选中本程序自身：走专用自卸载流程（确认 → 启动 uninst.exe → 退出），
    // 而不是去执行指向自身的卸载命令（那样会自我强杀，体验割裂且打断后续操作）。
    if (software->regPath == "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\UninstallerManager") {
        uninstallSelf();
        return;
    }

    // ⑦ 系统关键项拦截：Windows 更新 / 驱动 / 系统组件 等不允许卸载。
    if (isCriticalSystemItem(software)) {
        QMessageBox::critical(this, QString::fromUtf8(u8"无法卸载"),
            QString::fromUtf8(u8"「%1」被识别为系统关键项（Windows 更新 / 驱动 / 系统组件）。\n卸载它可能导致系统不稳定，已阻止该操作。").arg(QString::fromStdString(software->displayName)));
        return;
    }

    // 无卸载命令的条目（多为已判定为残留、卸载程序已不存在的注册表项）：
    // 继续走卸载只会令 uninstallSoftware 返回 Failed 并误报“卸载失败”。
    // 这类条目应改用「删除注册表项 / 强制删除此条目」清理，故提前友好拦截并提示。
    if (software->uninstallString.empty()) {
        QMessageBox::information(this, QString::fromUtf8(u8"无法卸载"),
            QString::fromUtf8(u8"「%1」没有可用的卸载命令（卸载程序可能已不存在）。\n"
                              "若确认要清理该条目，请改用右键菜单的「删除注册表项」或「强制删除此条目」。").arg(QString::fromStdString(software->displayName)));
        return;
    }

    // ⑦-1 卸载程序本体已不存在（残留项，isOrphaned）：执行卸载只会 ShellExecute 失败并误报“卸载失败”。
    // 引导用户用右键菜单的「强制删除此条目」清理，而不是空跑一次卸载。
    if (software->isOrphaned) {
        QMessageBox::information(this, QString::fromUtf8(u8"无法卸载"),
            QString::fromUtf8(u8"「%1」的卸载程序已不存在，这是一条残留条目（注册表项还在，但安装文件和卸载器已被删除）。\n\n"
                              "若要清理该条目，请右键该项的「删除注册表项」或「强制删除此条目」。").arg(QString::fromStdString(software->displayName)));
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

    UninstallResult result = doUninstall(software);

    if (result == UninstallResult::Success) {
        // 卸载成功后自动扫描残留（force=true 绕过 isOrphaned，正常卸载项也能检出
        // AppData/桌面等残留），有残留直接弹清理框，无残留仅状态栏提示，不再询问。
        autoScanResidualsAfterUninstall(software);
        // ② 重新扫描注册表，让列表反映真实状态（已卸载的条目会消失）
        built_list(false);
        loadSoftwareList();
    }
    else if (result == UninstallResult::Canceled) {
        // 用户主动取消（拒绝 UAC 提权 / 在向导里点取消）：未做任何更改，不应误报失败。
        QMessageBox::information(this, QString::fromUtf8(u8"已取消"),
            QString::fromUtf8(u8"「%1」的卸载已取消，未做任何更改。").arg(name));
    }
    else {
        QMessageBox::critical(this, getlang(0xFu).toString(), getlang(0x10u).toString());
    }

    m_busy = false;

    // 记录到操作历史
    if (result == UninstallResult::Success)
        logOperation(QString::fromUtf8(u8"卸载"), name, true, QString());
    else if (result == UninstallResult::Failed)
        logOperation(QString::fromUtf8(u8"卸载"), name, false, QString::fromUtf8(u8"卸载返回失败"));
}
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

    // ⑦ 过滤掉系统关键项 / 残留项 / 本程序自身，并分别给出提示
    QStringList blocked;
    QStringList orphaned;
    bool selfIncluded = false;
    QList<SoftwareInfo*> toUninstall;
    filesize_t totalSize;
    for (auto sw : selected) {
        // 本程序自身：不能在批量里卸载（会自我强杀、打断批次），引导用菜单「卸载本程序」。
        if (sw->regPath == "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\UninstallerManager") {
            selfIncluded = true; continue;
        }
        if (isCriticalSystemItem(sw)) { blocked.append(QString::fromStdString(sw->displayName)); continue; }
        // 残留项：卸载程序已不存在，批量执行只会误报失败；引导用右键「强制删除此条目」。
        if (sw->isOrphaned) { orphaned.append(QString::fromStdString(sw->displayName)); continue; }
        toUninstall.append(sw); totalSize += sw->size.size;
    }
    if (toUninstall.isEmpty()) {
        QString msg;
        if (selfIncluded) msg += QString::fromUtf8(u8"「卸载管理器」自身无法在批量中卸载，请用菜单「卸载本程序」。\n");
        if (!blocked.isEmpty()) msg += QString::fromUtf8(u8"已阻止 %1 个系统关键项（Windows 更新 / 驱动 等）。\n").arg(blocked.size());
        if (!orphaned.isEmpty()) msg += QString::fromUtf8(u8"已跳过 %1 个残留项，请用右键「强制删除此条目」清理。\n").arg(orphaned.size());
        QMessageBox::information(this, QString::fromUtf8(u8"无法批量卸载"), msg.trimmed());
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
    if (!orphaned.isEmpty()) {
        preview += QString::fromUtf8(u8"\n已跳过 %1 个残留项（卸载程序不存在，请用右键「强制删除此条目」）：\n").arg(orphaned.size());
        for (const QString& b : orphaned) preview += "• " + b + "\n";
    }
    if (selfIncluded) {
        preview += QString::fromUtf8(u8"\n已跳过「卸载管理器」自身（请用菜单「卸载本程序」）。\n");
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
    int ok = 0, canceled = 0, failed = 0;
    QList<SoftwareInfo*> succeeded;  // 记录成功卸载项，供后续自动残留清理
    // 批次级系统还原点（覆盖整个批次，失败静默跳过）
    createSystemRestorePoint(QString::fromUtf8(u8"批量卸载 %1 个程序").arg(toUninstall.size()));
    // 单一总进度条：逐条目推进，避免每个卸载项都弹独立对话框、体验割裂。
    QProgressDialog master(QString::fromUtf8(u8"正在批量卸载…"), getlang(0x3u).toString(),
        0, static_cast<int>(toUninstall.size()), this);
    master.setWindowModality(Qt::WindowModal);
    master.setMinimumDuration(0);
    master.show();
    for (int i = 0; i < toUninstall.size(); ++i) {
        master.setValue(i);
        master.setLabelText(QString::fromUtf8(u8"正在卸载：%1 (%2/%3)")
            .arg(QString::fromStdString(toUninstall[i]->displayName))
            .arg(i + 1).arg(toUninstall.size()));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        UninstallResult r = doUninstall(toUninstall[i], /*showProgress=*/false);
        if (r == UninstallResult::Success) {
            ++ok; succeeded.append(toUninstall[i]);
            logOperation(QString::fromUtf8(u8"批量卸载"),
                         QString::fromStdString(toUninstall[i]->displayName), true, QString());
        }
        else if (r == UninstallResult::Canceled) ++canceled;
        else { ++failed;
            logOperation(QString::fromUtf8(u8"批量卸载"),
                         QString::fromStdString(toUninstall[i]->displayName), false,
                         QString::fromUtf8(u8"卸载返回失败"));
        }
    }
    master.setValue(static_cast<int>(toUninstall.size()));
    master.close();
    m_busy = false;

    QString msg = QString::fromUtf8(u8"成功卸载 %1 / %2 个程序。").arg(ok).arg(toUninstall.size());
    if (canceled > 0) {
        msg += QString::fromUtf8(u8"\n已取消 %1 个（UAC 提权被拒或向导取消，未做更改）。").arg(canceled);
    }
    if (failed > 0) {
        msg += QString::fromUtf8(u8"\n失败 %1 个。").arg(failed);
    }
    QMessageBox::information(this, QString::fromUtf8(u8"批量卸载完成"), msg);

    // ② 卸载完成后自动汇总残留并弹清理对话框（无需逐条询问）
    if (!succeeded.isEmpty()) {
        showBatchResidualCleanup(succeeded);
    }

    // ② 统一重扫（强制重扫，确保已卸载条目立即从列表消失）
    built_list(false);
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
    // 使用 System32 下的完整路径，避免应用目录被种植同名二进制（F4）。
    wchar_t sysDir[MAX_PATH] = {0};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    QString sysPath = sysDir[0] ? QString::fromWCharArray(sysDir) : QStringLiteral("C:\\Windows\\System32");
    QString taskkill = sysPath + QStringLiteral("\\taskkill.exe");
    QString regedit  = sysPath + QStringLiteral("\\regedit.exe");
    QProcess::execute(taskkill, QStringList() << "/IM" << "regedit.exe" << "/F");
    QSettings lastKey("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Applets\\Regedit",
                      QSettings::NativeFormat);
    lastKey.setValue("LastKey", key);
    lastKey.sync();  // 确保 regedit 启动前 LastKey 已落盘，避免读到旧值/跳错位置
    QProcess::startDetached(regedit);
}

// ⑨ 切换亮/暗主题并持久化。
void UninstallerWindow::setTheme(int t) {
    m_theme = t;
    QSettings s("Uninstaller", "uninstaller");
    s.setValue("theme", t);
    s.sync();  // closeEvent 强制退出不跑析构，设置必须立即落盘

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
    // 自定义背景：追加半透明化规则（QSS 后写覆盖先写，仅替换背景色，其余样式不动）
    if (!m_bgPixmap.isNull()) sheet += bgOverlaySheet();
    qApp->setStyleSheet(sheet);
    // 强制刷新所有已打开窗口的样式
    for (QWidget* w : QApplication::allWidgets()) {
        if (w) { w->style()->unpolish(w); w->style()->polish(w); }
    }

    // 同步视图菜单的勾选状态（互斥单选，唯一选中当前主题）
    if (m_lightThemeAct) m_lightThemeAct->setChecked(t == 0);
    if (m_darkThemeAct) m_darkThemeAct->setChecked(t == 1);
    syncBgMenuChecks();
    update();  // 有背景图时触发 paintEvent 重画
}

// —— 自定义背景 ——————————————————————————————————————————

// 背景图片的持久化位置（AppData/Local/<应用名>/custom_bg.png）。
// 选择图片后另存一份到这里：原文件被移动/删除/换盘后背景依然有效。
static QString customBgFilePath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
         + QStringLiteral("/custom_bg.png");
}

// 有背景图时追加到主题 QSS 的规则：中央控件与主要面板转半透明，
// 让 paintEvent 画出的背景透出，同时保留足够底色保证文字可读。
QString UninstallerWindow::bgOverlaySheet() const {
    if (m_theme == 1) {
        return QString::fromUtf8(
            "QWidget#centralWidget { background: transparent; }"
            "QTableWidget { background-color: rgba(32,35,40,170); alternate-background-color: rgba(38,42,49,170); gridline-color: rgba(255,255,255,24); }"
            "QTableWidget::item:hover { background-color: rgba(74,144,226,90); }"
            "QTableWidget::item:selected { background-color: rgba(74,144,226,150); color:#ffffff; }"
            "QHeaderView::section { background-color: rgba(32,35,40,180); color:#e0e3e8; border:none; border-bottom:2px solid rgba(255,255,255,30); }"
            "QMenuBar { background-color: rgba(32,35,40,150); }"
            "QLineEdit { background-color: rgba(32,35,40,190); }");
    }
        return QString::fromUtf8(
            "QWidget#centralWidget { background: transparent; }"
            "QTableWidget { background-color: rgba(255,255,255,170); alternate-background-color: rgba(244,246,248,170); gridline-color: rgba(0,0,0,24); }"
            "QTableWidget::item:hover { background-color: rgba(74,144,226,80); }"
            "QTableWidget::item:selected { background-color: rgba(74,144,226,150); color:#ffffff; }"
            "QHeaderView::section { background-color: rgba(242,242,242,180); color:#1a1a1a; border:none; border-bottom:2px solid rgba(0,0,0,30); }"
            "QMenuBar { background-color: rgba(242,242,242,150); }"
            "QLineEdit { background-color: rgba(255,255,255,190); }");
}

void UninstallerWindow::loadCustomBgSettings() {
    QSettings s(QStringLiteral("Uninstaller"), QStringLiteral("uninstaller"));
    m_bgDim = qBound(0, s.value(QStringLiteral("bgDim"), 1).toInt(), 2);
    m_bgMode = qBound(0, s.value(QStringLiteral("bgMode"), 0).toInt(), 2);
    m_bgBlur = s.value(QStringLiteral("bgBlur"), false).toBool();
    m_bgFollowDesktop = s.value(QStringLiteral("bgFollowDesktop"), false).toBool();
    if (m_bgFollowDesktop) { loadDesktopWallpaper(); return; }
    if (!s.value(QStringLiteral("customBg"), false).toBool()) return;
    QImage img(customBgFilePath());
    if (img.isNull()) return;  // 图片文件丢失（如手动清理 AppData）：静默回退默认背景
    m_bgPixmap = QPixmap::fromImage(img);
}

// 读取系统桌面壁纸路径并加载为背景（视图菜单“使用系统桌面壁纸”）。
// 纯色背景或读取失败：m_bgPixmap 保持空，paintEvent 回退默认样式。
void UninstallerWindow::loadDesktopWallpaper() {
    wchar_t path[MAX_PATH] = {0};
    m_bgPixmap = QPixmap();
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0) && path[0]) {
        QImage img(QString::fromWCharArray(path));
        if (img.isNull()) return;
        if (img.width() > 2560 || img.height() > 1600)
            img = img.scaled(2560, 1600, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_bgPixmap = QPixmap::fromImage(img);
        m_bgScaled = QPixmap(); m_bgScaledFor = QSize(); m_bgCenterCache = QPixmap();
    }
}

// 背景模糊：先降采样 1/4，再做一次 3x3 盒式模糊，最后平滑放大回原尺寸。
// 模糊开销降 16 倍，放大自带柔和效果——避免大图逐像素模糊卡死 UI 线程
// （实测对 2560x1600 原图直接两次盒式模糊会让 paintEvent 卡住数分钟，窗口黑屏）。
QPixmap UninstallerWindow::blurPixmap(const QPixmap& src) const {
    if (src.isNull()) return src;
    QImage small = src.toImage().convertToFormat(QImage::Format_RGB32)
        .scaled(qMax(1, src.width() / 4), qMax(1, src.height() / 4),
                Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const int w = small.width(), h = small.height();
    QImage tmp(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int r = 0, g = 0, b = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                const int yy = qBound(0, y + dy, h - 1);
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = qBound(0, x + dx, w - 1);
                    const QRgb p = small.pixel(xx, yy);
                    r += qRed(p); g += qGreen(p); b += qBlue(p);
                }
            }
            tmp.setPixel(x, y, qRgb(r / 9, g / 9, b / 9));
        }
    }
    return QPixmap::fromImage(
        tmp.scaled(src.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
}

void UninstallerWindow::setCustomBackground() {
    const QString path = QFileDialog::getOpenFileName(
        this, QString::fromUtf8(u8"选择背景图片"), QDir::homePath(),
        QString::fromUtf8(u8"图片 (*.png *.jpg *.jpeg *.bmp *.webp)"));
    if (path.isEmpty()) return;
    QImage img(path);
    if (img.isNull()) {
        QMessageBox::warning(this, QString::fromUtf8(u8"自定义背景"),
            QString::fromUtf8(u8"无法读取该图片文件，请换一张试试。"));
        return;
    }
    // 超大图先降采样，避免内存与每次 resize 的缩放开销（2560 宽对 4K 屏足够）
    if (img.width() > 2560 || img.height() > 1600) {
        img = img.scaled(2560, 1600, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    const QString dst = customBgFilePath();
    QDir().mkpath(QFileInfo(dst).absolutePath());
    if (!img.save(dst, "PNG")) {
        QMessageBox::warning(this, QString::fromUtf8(u8"自定义背景"),
            QString::fromUtf8(u8"背景图片保存失败。"));
        return;
    }
    m_bgPixmap = QPixmap::fromImage(img);
    m_bgScaled = QPixmap();
    m_bgScaledFor = QSize();
    QSettings s(QStringLiteral("Uninstaller"), QStringLiteral("uninstaller"));
    s.setValue(QStringLiteral("customBg"), true);
    s.sync();  // 强退不跑析构，立即落盘
    setTheme(m_theme);  // 重新套样式（含透明化附加段）并重画
}

void UninstallerWindow::clearCustomBackground() {
    QFile::remove(customBgFilePath());
    m_bgPixmap = QPixmap();
    m_bgScaled = QPixmap();
    m_bgScaledFor = QSize();
    m_bgCenterCache = QPixmap();
    m_bgFollowDesktop = false;
    QSettings s(QStringLiteral("Uninstaller"), QStringLiteral("uninstaller"));
    s.setValue(QStringLiteral("customBg"), false);
    s.setValue(QStringLiteral("bgFollowDesktop"), false);
    s.sync();
    setTheme(m_theme);  // 恢复不透明默认样式
}

void UninstallerWindow::setBgDim(int level) {
    m_bgDim = qBound(0, level, 2);
    QSettings s(QStringLiteral("Uninstaller"), QStringLiteral("uninstaller"));
    s.setValue(QStringLiteral("bgDim"), m_bgDim);
    s.sync();
    syncBgMenuChecks();
    update();
}

void UninstallerWindow::setBgMode(int mode) {
    m_bgMode = qBound(0, mode, 2);
    QSettings s(QStringLiteral("Uninstaller"), QStringLiteral("uninstaller"));
    s.setValue(QStringLiteral("bgMode"), m_bgMode);
    s.sync();
    m_bgScaled = QPixmap(); m_bgScaledFor = QSize(); m_bgCenterCache = QPixmap();
    syncBgMenuChecks();
    update();
}

void UninstallerWindow::setBgBlur(bool on) {
    m_bgBlur = on;
    QSettings s(QStringLiteral("Uninstaller"), QStringLiteral("uninstaller"));
    s.setValue(QStringLiteral("bgBlur"), m_bgBlur);
    s.sync();
    m_bgScaled = QPixmap(); m_bgScaledFor = QSize(); m_bgCenterCache = QPixmap();
    syncBgMenuChecks();
    update();
}

void UninstallerWindow::useDesktopWallpaper() {
    loadDesktopWallpaper();
    if (m_bgPixmap.isNull()) {
        QMessageBox::information(this, QString::fromUtf8(u8"背景"),
            QString::fromUtf8(u8"未检测到系统桌面壁纸（可能是纯色背景），已保持当前背景。"));
        return;
    }
    m_bgFollowDesktop = true;
    QSettings s(QStringLiteral("Uninstaller"), QStringLiteral("uninstaller"));
    s.setValue(QStringLiteral("bgFollowDesktop"), true);
    s.setValue(QStringLiteral("customBg"), false);
    s.sync();
    setTheme(m_theme);  // 重新套透明化样式并重画
}

void UninstallerWindow::syncBgMenuChecks() {
    for (int i = 0; i < 3; ++i) {
        if (m_bgDimActs[i]) m_bgDimActs[i]->setChecked(i == m_bgDim);
        if (m_bgModeActs[i]) m_bgModeActs[i]->setChecked(i == m_bgMode);
    }
    if (m_bgBlurAct) m_bgBlurAct->setChecked(m_bgBlur);
    if (m_bgDesktopAct) m_bgDesktopAct->setChecked(m_bgFollowDesktop);
    if (m_bgClearAct) m_bgClearAct->setEnabled(!m_bgPixmap.isNull());
}

// 背景绘制：cover 模式铺满窗口（居中裁剪），再叠一层主题色遮罩。
// 淡/中/浓 三档遮罩 alpha：背景越明显文字对比度越低，浓档基本只留隐约纹理。
// 有背景时不调基类 paintEvent（否则 QSS 的窗口不透明底色会盖掉图片）；
// 子控件（半透明表格/菜单栏/搜索框）自行绘制，透明度由 bgOverlaySheet 控制。
void UninstallerWindow::paintEvent(QPaintEvent* event) {
    if (m_bgPixmap.isNull()) {
        QMainWindow::paintEvent(event);
        return;
    }
    QPainter p(this);
    const QSize ws = size();
    if (m_bgMode == 2) {
        // 平铺：缩小到合适尺寸避免大图过抢眼；平铺不模糊
        QPixmap tile = m_bgPixmap.scaledToWidth(qMin(256, m_bgPixmap.width()),
                                                Qt::SmoothTransformation);
        p.drawTiledPixmap(rect(), tile);
    } else if (m_bgMode == 1) {
        // 居中：原图 1:1 绘制（不拉伸），超出窗口部分裁掉
        QPixmap draw = m_bgPixmap;
        if (m_bgBlur) {
            if (m_bgCenterCache.isNull()) m_bgCenterCache = blurPixmap(m_bgPixmap);
            draw = m_bgCenterCache;
        }
        const int x = qMax(0, (ws.width() - draw.width()) / 2);
        const int y = qMax(0, (ws.height() - draw.height()) / 2);
        p.drawPixmap(x, y, draw);
    } else {
        // 铺满(cover)：缩放铺满窗口（居中裁剪），模糊时柔化
        if (m_bgScaledFor != ws || m_bgScaled.isNull()) {
            m_bgScaled = m_bgPixmap.scaled(ws, Qt::KeepAspectRatioByExpanding,
                                           Qt::SmoothTransformation);
            if (m_bgBlur) m_bgScaled = blurPixmap(m_bgScaled);
            m_bgScaledFor = ws;
        }
        const int sx = qMax(0, (m_bgScaled.width() - ws.width()) / 2);
        const int sy = qMax(0, (m_bgScaled.height() - ws.height()) / 2);
        p.drawPixmap(0, 0, ws.width(), ws.height(), m_bgScaled, sx, sy, ws.width(), ws.height());
    }
    QColor overlay = (m_theme == 1) ? QColor(24, 26, 30) : QColor(255, 255, 255);
    static const int kDimAlpha[3] = { 110, 165, 210 };  // 淡 / 中 / 浓
    overlay.setAlpha(kDimAlpha[m_bgDim]);
    p.fillRect(rect(), overlay);
}

void UninstallerWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    m_bgScaledFor = QSize();  // 失效背景缓存，下次 paint 重算
    update();
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
        // ② 重扫（强制重扫，确保已删除残留立即从列表消失）
        built_list(false);
        loadSoftwareList();
    } else {
        // 部分文件可能因占用/无权限删除失败：重新汇总仍存在的残留并给出诊断，
        // 让用户知道“已删哪些、还剩哪些”，而不是只看到一条笼统失败。
        std::vector<std::string> remain;
        for (auto sw : selected) {
            auto r = Registry::scanResidualFiles(*sw, sw->isOrphaned);
            for (const auto& f : r) remain.push_back(f);
        }
        QString hint = informat::diagnoseDeleteFailure(allResiduals);
        QString msg = getlang(0x1A).toString() + "\n\n" + hint;
        const int deleted = static_cast<int>(allResiduals.size()) - static_cast<int>(remain.size());
        if (deleted > 0 && !remain.empty()) {
            msg += QString::fromUtf8(u8"\n\n已删除 %1 个，仍有 %2 个残留未删除（可能被占用或无权限）。")
                       .arg(deleted).arg(static_cast<int>(remain.size()));
        } else if (deleted > 0 && remain.empty()) {
            msg += QString::fromUtf8(u8"\n\n已删除 %1 个残留。").arg(deleted);
        }
        QMessageBox::warning(this, getlang(0xFu).toString(), msg);
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
    QAction* actLocate = menu.addAction(getlang(0x4D).toString());
    QAction* actDelReg = menu.addAction(getlang(0x4E).toString());
    QAction* actForceDel = menu.addAction(getlang(0x4F).toString());

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

    menu.addSeparator();
    QAction* actExportSel = menu.addAction(QString::fromUtf8(u8"导出选中软件…"));
    connect(actExportSel, &QAction::triggered, this, [this, row]() {
        m_tableWidget->setCurrentCell(row, 0);
        // 右击行若未被选中，则加入选中（保留已有多选），保证「导出选中」至少含本行
        QItemSelectionModel* sm = m_tableWidget->selectionModel();
        if (sm && !sm->isSelected(m_tableWidget->model()->index(row, 0)))
            sm->select(m_tableWidget->model()->index(row, 0),
                       QItemSelectionModel::Select | QItemSelectionModel::Rows);
        exportSelectedSoftware();
    });

    menu.exec(m_tableWidget->viewport()->mapToGlobal(pos));
}

void UninstallerWindow::scanResiduals() {
    int row = m_tableWidget->currentRow();
    if (tick(row))return;

    auto software = softwareAtRow(row);

    // 正常安装/运行的软件不是残留项（孤儿项），扫描残留本就没有意义：
    // scanResidualFiles 内部对 !isOrphaned 直接返回空，若仍走完流程会弹出模糊的“未找到”。
    // 这里先给出明确说明，避免用户误以为“扫描失败/找不到文件”。
    if (!software->isOrphaned) {
        QMessageBox::information(this, getlang(0x12).toString(), getlang(0x54).toString());
        return;
    }

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

    // 残留项（孤儿项）扫描结果直接用清理对话框展示（force=false，重扫沿用孤儿判定）。
    showResidualCleanup(software, residuals, /*force=*/false);
}

// 残留清理对话框：列出残留文件/目录并提供一键清理。force 控制清理后“重扫反映剩余”时
// 是否绕过 isOrphaned（自动残留检测传 true，手动孤儿项扫描传 false）。
void UninstallerWindow::showResidualCleanup(SoftwareInfo* software,
                                            const std::vector<std::string>& residuals,
                                            bool force) {
    if (!software || residuals.empty()) return;

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
            logOperation(QString::fromUtf8(u8"删除残留"),
                         QString::fromStdString(software->displayName), true,
                         QString::fromUtf8(u8"%1 个文件/目录").arg(residuals.size()));
            dialog.accept();
            // ② 删除成功后重新扫描，让列表反映真实状态（强制重扫）
            built_list(false);
            loadSoftwareList();
        }
        else {
            // ④ C 盘被锁定/无权限/文件占用时，给出可操作的友好提示，而非笼统报错。
            auto remain = Registry::scanResidualFiles(*software, force);
            QString hint = informat::diagnoseDeleteFailure(residuals);
            QString msg = getlang(0x1A).toString() + "\n\n" + hint;
            const int deleted = static_cast<int>(residuals.size()) - static_cast<int>(remain.size());
            if (deleted > 0 && !remain.empty()) {
                msg += QString::fromUtf8(u8"\n\n已删除 %1 个，仍有 %2 个残留未删除（可能被占用或无权限）。")
                           .arg(deleted).arg(static_cast<int>(remain.size()));
            } else if (deleted > 0 && remain.empty()) {
                msg += QString::fromUtf8(u8"\n\n已删除 %1 个残留。").arg(deleted);
            }
            QMessageBox::warning(&dialog, getlang(0xF).toString(), msg);
        }
        });

    connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);

    dialog.exec();
}

// 卸载成功后自动扫描残留：force=true 绕过 isOrphaned，使“正常卸载的程序”也能检出
// AppData/开始菜单/桌面等残留（否则 scanResidualFiles 因 !isOrphaned 直接返回空）。
// 有残留则弹清理对话框，无残留仅状态栏提示，不再弹“是否扫描”询问。
void UninstallerWindow::autoScanResidualsAfterUninstall(SoftwareInfo* software) {
    if (!software) return;
    auto residuals = Registry::scanResidualFiles(*software, /*force=*/true);
    if (residuals.empty()) {
        statusBar()->showMessage(
            QString::fromUtf8(u8"「%1」卸载完成，未发现磁盘残留。")
                .arg(QString::fromStdString(software->displayName)), 4000);
        return;
    }
    showResidualCleanup(software, residuals, /*force=*/true);
}

// 批量卸载完成后的合并残留清理：汇总所有成功卸载项的残留，弹一个统一对话框，
// 提供「一键清理全部」。指针有效期在 built_list 重建列表之前，调用时机已保证安全。
void UninstallerWindow::showBatchResidualCleanup(const QList<SoftwareInfo*>& uninstalled) {
    std::vector<std::string> all;
    for (auto sw : uninstalled) {
        auto r = Registry::scanResidualFiles(*sw, /*force=*/true);
        for (const auto& p : r) all.push_back(p);
    }
    if (all.empty()) {
        statusBar()->showMessage(QString::fromUtf8(u8"已卸载的软件未发现磁盘残留。"), 4000);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QString::fromUtf8(u8"卸载残留清理"));
    dialog.resize(640, 420);
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QTextEdit* textEdit = new QTextEdit(&dialog);
    textEdit->setReadOnly(true);
    QString content = QString::fromUtf8(u8"以下 %1 个残留文件/目录来自刚卸载的软件，确认后一并清理：\n\n").arg(all.size());
    for (const auto& p : all) content += QString::fromStdString(p) + "\n";
    textEdit->setText(content);
    layout->addWidget(textEdit);

    QPushButton* cleanBtn = new QPushButton(QString::fromUtf8(u8"一键清理全部"), &dialog);
    QPushButton* cancelBtn = new QPushButton(getlang(0x3).toString(), &dialog);
    QHBoxLayout* btnLayout = new QHBoxLayout();
    btnLayout->addWidget(cleanBtn);
    btnLayout->addWidget(cancelBtn);
    layout->addLayout(btnLayout);

    connect(cleanBtn, &QPushButton::clicked, [&]() {
        QMessageBox::StandardButton confirm = QMessageBox::question(
            &dialog, QString::fromUtf8(u8"确认清理残留"),
            QString::fromUtf8(u8"即将删除上面列出的 %1 个残留文件/目录，此操作不可撤销。\n确定继续吗？").arg(all.size()),
            QMessageBox::Yes | QMessageBox::No);
        if (confirm != QMessageBox::Yes) return;
        if (Registry::deleteResidualFiles(all)) {
            QMessageBox::information(&dialog, getlang(0xD).toString(), getlang(0x19).toString());
            logOperation(QString::fromUtf8(u8"批量删除残留"),
                         QString::fromUtf8(u8"多软件"), true,
                         QString::fromUtf8(u8"%1 个文件/目录").arg(all.size()));
            dialog.accept();
            built_list(false);
            loadSoftwareList();
        } else {
            QString hint = informat::diagnoseDeleteFailure(all);
            QMessageBox::warning(&dialog, getlang(0xF).toString(), getlang(0x1A).toString() + "\n\n" + hint);
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

    // 系统关键组件（Windows 更新/驱动/运行库等）不可删除，与卸载路径保持一致的拦截（P1）。
    if (isCriticalSystemItem(sw)) {
        // R6（删键被拒）：系统关键组件删除被拦截，记录结构化安全事件。
        logSecurityEvent(QStringLiteral("R6"),
                         QStringLiteral("registry_delete_blocked"),
                         QString::fromUtf8(u8"拦截删除系统关键组件：%1 @ %2").arg(name).arg(regPath));
        QMessageBox::critical(this, QString::fromUtf8(u8"禁止删除"),
            QString::fromUtf8(u8"该条目为系统关键组件（如 Windows 更新 / 驱动 / 运行库），删除可能导致系统功能异常，已阻止。"));
        return;
    }

    if (m_busy) return;
    m_busy = true;
    bool ok = Registry::deleteRegistryKey(sw->hive, sw->regPath);
    m_busy = false;

    if (ok) {
        QMessageBox::information(this, QString::fromUtf8(u8"成功"), QString::fromUtf8(u8"注册表项已删除。"));
        logOperation(QString::fromUtf8(u8"删除注册表项"), name, true, QString());
        // 重新扫描注册表，让列表反映真实状态（残留条目会消失，强制重扫）
        built_list(false);
        loadSoftwareList();
    }
    else {
        QMessageBox::critical(this, QString::fromUtf8(u8"失败"),
            QString::fromUtf8(u8"删除注册表项失败。\n若为 HKLM 项，可能被安全软件拦截或需要管理员权限；若为 HKCU 项，则该项可能已被删除。"));
        logOperation(QString::fromUtf8(u8"删除注册表项"), name, false, QString::fromUtf8(u8"删除失败"));
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

    // 即便“强制删除”，系统关键组件（Windows 更新/驱动/运行库）也绝不可删，避免破坏系统（P1）。
    if (isCriticalSystemItem(sw)) {
        // R6（删键被拒）：强制删除路径下系统关键组件仍被拦截，记录结构化安全事件。
        logSecurityEvent(QStringLiteral("R6"),
                         QStringLiteral("registry_delete_blocked"),
                         QString::fromUtf8(u8"强制删除中拦截系统关键组件：%1 @ %2").arg(name).arg(regPath));
        QMessageBox::critical(this, QString::fromUtf8(u8"禁止删除"),
            QString::fromUtf8(u8"该条目为系统关键组件（如 Windows 更新 / 驱动 / 运行库），即使强制删除也已阻止。"));
        return;
    }

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
        logOperation(QString::fromUtf8(u8"强制删除"), name, true,
                     QString::fromUtf8(u8"注册表项 + %1 个磁盘残留").arg(residuals.size()));
        // 重新扫描注册表，让列表反映真实状态（该条目会消失，强制重扫）
        built_list(false);
        loadSoftwareList();
    }
    else {
        QMessageBox::critical(this, QString::fromUtf8(u8"失败"),
            QString::fromUtf8(u8"删除注册表项失败。\n若为 HKLM 项，可能被安全软件拦截或需要管理员权限；若为 HKCU 项，则该项可能已被删除。"));
        logOperation(QString::fromUtf8(u8"强制删除"), name, false, QString::fromUtf8(u8"删除注册表项失败"));
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
        getlang(0x40).toString() + QString::fromUtf8(u8" 卸载管理器"),
        QString::fromUtf8(u8"卸载管理器\n版本 %1\n\n"
            "一款用于查看、卸载与清理 Windows 已安装软件及残留项的工具。\n"
            "基于 Qt 6 与 C++ 构建。").arg(appVersionFull()));
}

// —— 磁盘占用排行：自绘条形图（Top15），点击条目跳转并选中列表对应行 ——
// 不继承 Q_OBJECT，点击用 std::function 回调，避免 .cpp 内 moc 负担。
class SizeRankWidget : public QWidget {
public:
    struct Bar { QString name; long double bytes; QString label; SoftwareInfo* sw; };
    using PickFn = std::function<void(SoftwareInfo*)>;
    SizeRankWidget(QVector<Bar> bars, PickFn pick, QWidget* parent = nullptr)
        : QWidget(parent), m_bars(std::move(bars)), m_pick(std::move(pick)) {
        setMouseTracking(true);
        setMinimumSize(560, 40 + m_bars.size() * 26);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const int rowH = 26, nameW = 190, pad = 10;
        if (m_bars.isEmpty()) return;
        long double mx = m_bars.first().bytes;
        if (mx <= 0) mx = 1;
        const int barMaxW = std::max(60, width() - nameW - pad * 2 - 70);
        for (int i = 0; i < m_bars.size(); ++i) {
            const Bar& b = m_bars[i];
            int y = pad + i * rowH;
            QFontMetrics fm(font());
            p.setPen(palette().color(QPalette::Text));
            // 软件名（左，超宽省略）
            QFont f = font();
            QFontMetrics nameFm(f);
            QString disp = nameFm.elidedText(b.name, Qt::ElideRight, nameW - pad);
            p.drawText(pad, y, nameW - pad, rowH, Qt::AlignLeft | Qt::AlignVCenter, disp);
            // 条形
            int w = int(double(b.bytes) / double(mx) * barMaxW);
            w = std::max(w, 3);
            QColor c = (i == m_hover) ? QColor(0x4A, 0x90, 0xE2) : QColor(0x3C, 0x8A, 0xE0);
            p.setBrush(c);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(QRectF(nameW, y + 5, w, rowH - 12), 4, 4);
            // 右侧大小标签
            p.setPen(palette().color(QPalette::Text));
            p.drawText(nameW + w + 6, y, 64, rowH, Qt::AlignLeft | Qt::AlignVCenter, b.label);
        }
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        const int rowH = 26, pad = 10;
        int idx = (e->position().y() - pad) / rowH;
        if (idx < 0 || idx >= m_bars.size()) idx = -1;
        if (idx != m_hover) { m_hover = idx; update(); }
    }
    void leaveEvent(QEvent* e) override { QWidget::leaveEvent(e); if (m_hover != -1) { m_hover = -1; update(); } }
    void mousePressEvent(QMouseEvent* e) override {
        if (m_hover >= 0 && m_hover < m_bars.size() && m_pick) m_pick(m_bars[m_hover].sw);
    }
private:
    QVector<Bar> m_bars;
    PickFn m_pick;
    int m_hover{ -1 };
};

void UninstallerWindow::jumpToSoftware(SoftwareInfo* sw) {
    if (!sw || !m_tableWidget) return;
    for (int r = 0; r < m_tableWidget->rowCount(); ++r) {
        if (softwareAtRow(r) == sw) {
            m_tableWidget->setCurrentCell(r, 0);
            m_tableWidget->scrollToItem(m_tableWidget->item(r, 0), QAbstractItemView::PositionAtCenter);
            m_tableWidget->setFocus();
            return;
        }
    }
}

void UninstallerWindow::showSizeRanking() {
    // 汇总当前可见软件的大小，取 Top15
    QVector<SoftwareInfo*> all;
    for (int r = 0; r < m_tableWidget->rowCount(); ++r) {
        if (m_tableWidget->isRowHidden(r)) continue;
        if (auto s = softwareAtRow(r)) all.append(s);
    }
    std::sort(all.begin(), all.end(), [](SoftwareInfo* a, SoftwareInfo* b) {
        return a->size.size > b->size.size;
    });
    const int TOP = 15;
    QVector<SizeRankWidget::Bar> bars;
    int n = std::min<int>(TOP, (int)all.size());
    for (int i = 0; i < n; ++i) {
        SoftwareInfo* s = all[i];
        bars.append({ QString::fromStdString(s->displayName), (long double)s->size.size,
                      QString::fromStdString(s->size.get()), s });
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QString::fromUtf8(u8"磁盘占用排行 Top %1").arg(bars.size()));
    int dlgH = std::min(760, 120 + (int)bars.size() * 26);
    dlg.resize(620, dlgH);
    QVBoxLayout* root = new QVBoxLayout(&dlg);
    QLabel* hint = new QLabel(QString::fromUtf8(u8"按“估算安装大小”排序，点击条目可跳转到列表对应行。"));
    hint->setStyleSheet("color:#9da3ad;");
    root->addWidget(hint);
    auto* w = new SizeRankWidget(bars, [this, &dlg](SoftwareInfo* sw) {
        jumpToSoftware(sw);
        dlg.accept();
    }, &dlg);
    root->addWidget(w);
    QHBoxLayout* btns = new QHBoxLayout();
    btns->addStretch();
    QPushButton* closeBtn = new QPushButton(QString::fromUtf8(u8"关闭"), &dlg);
    btns->addWidget(closeBtn);
    root->addLayout(btns);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    dlg.exec();
}

// 全局残留体检：扫描所有孤儿（残留）项对应的磁盘残留，提供一键批量清理。
void UninstallerWindow::scanAllResiduals() {
    // 收集所有残留项（isOrphaned）
    std::vector<SoftwareInfo*> orphans;
    for (auto& s : m_softwareList) if (s.isOrphaned) orphans.push_back(&s);
    if (orphans.empty()) {
        QMessageBox::information(this, QString::fromUtf8(u8"残留体检"),
            QString::fromUtf8(u8"未发现残留项。所有已注册软件的卸载程序都仍然存在。"));
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QProgressDialog prog(QString::fromUtf8(u8"正在扫描残留…"), QString::fromUtf8(u8"取消"), 0, (int)orphans.size(), this);
    prog.setWindowModality(Qt::WindowModal);
    prog.setMinimumDuration(300);
    // 汇总：软件名 -> 其残留路径
    struct Item { QString name; QStringList paths; };
    QVector<Item> found;
    for (size_t i = 0; i < orphans.size(); ++i) {
        if (prog.wasCanceled()) break;
        SoftwareInfo* sw = orphans[i];
        // force=true：残留项 isOrphaned 已满足，但统一 force 覆盖边界情形
        auto paths = Registry::scanResidualFiles(*sw, /*force=*/true);
        if (!paths.empty()) {
            Item it;
            it.name = sw->displayName.empty() ? QString::fromUtf8(u8"(未命名)") : QString::fromStdString(sw->displayName);
            for (const auto& p : paths) it.paths << QString::fromStdString(p);
            found.append(it);
        }
        prog.setValue((int)i + 1);
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
    prog.close();
    QApplication::restoreOverrideCursor();

    if (prog.wasCanceled()) {
        QMessageBox::information(this, QString::fromUtf8(u8"残留体检"),
            QString::fromUtf8(u8"扫描已取消。"));
        return;
    }
    if (found.isEmpty()) {
        QMessageBox::information(this, QString::fromUtf8(u8"残留体检"),
            QString::fromUtf8(u8"共检查 %1 个残留项，未在磁盘上发现可清理的残留文件/目录。").arg((int)orphans.size()));
        return;
    }

    int totalPaths = 0;
    for (const auto& it : found) totalPaths += it.paths.size();

    QDialog dlg(this);
    dlg.setWindowTitle(QString::fromUtf8(u8"残留体检结果"));
    dlg.resize(660, 480);
    QVBoxLayout* lay = new QVBoxLayout(&dlg);
    QLabel* sum = new QLabel(QString::fromUtf8(u8"在 %1 个残留项中发现 %2 个残留文件/目录：").arg(found.size()).arg(totalPaths));
    sum->setStyleSheet("font-weight:bold;");
    lay->addWidget(sum);
    QTextEdit* te = new QTextEdit(&dlg);
    te->setReadOnly(true);
    QString content;
    for (const auto& it : found) {
        content += QString::fromUtf8(u8"■ %1（%2 项）\n").arg(it.name).arg(it.paths.size());
        for (const auto& p : it.paths) content += "    " + p + "\n";
    }
    te->setPlainText(content);
    lay->addWidget(te);
    QHBoxLayout* btns = new QHBoxLayout();
    QPushButton* cleanBtn = new QPushButton(QString::fromUtf8(u8"一键清理全部"), &dlg);
    QPushButton* cancelBtn = new QPushButton(QString::fromUtf8(u8"关闭"), &dlg);
    btns->addWidget(cleanBtn);
    btns->addStretch();
    btns->addWidget(cancelBtn);
    lay->addLayout(btns);
    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    connect(cleanBtn, &QPushButton::clicked, &dlg, [&]() {
        QMessageBox::StandardButton ok = QMessageBox::question(&dlg,
            QString::fromUtf8(u8"确认清理"),
            QString::fromUtf8(u8"即将删除上面列出的 %1 个残留文件/目录，此操作不可撤销。\n确定继续吗？").arg(totalPaths),
            QMessageBox::Yes | QMessageBox::No);
        if (ok != QMessageBox::Yes) return;
        // 汇总所有路径后统一删除（deleteResidualFiles 内部逐个安全删除并跳过受保护路径）
        std::vector<std::string> all;
        for (const auto& it : found)
            for (const auto& p : it.paths) all.push_back(p.toStdString());
        if (Registry::deleteResidualFiles(all)) {
            QMessageBox::information(&dlg, QString::fromUtf8(u8"完成"),
                QString::fromUtf8(u8"已清理 %1 个残留文件/目录。").arg(totalPaths));
            dlg.accept();
            logOperation(QString::fromUtf8(u8"全局残留清理"), QString::fromUtf8(u8"(全部残留项)"), true,
                         QString::fromUtf8(u8"清理 %1 项").arg(totalPaths));
            // 强制重扫，让列表反映真实状态
            built_list(false);
            loadSoftwareList();
        } else {
            logOperation(QString::fromUtf8(u8"全局残留清理"), QString::fromUtf8(u8"(全部残留项)"), false,
                         QString::fromUtf8(u8"部分删除失败"));
            QMessageBox::warning(&dlg, QString::fromUtf8(u8"部分失败"),
                QString::fromUtf8(u8"部分残留未能删除（可能被占用或无权限）。\n\n%1")
                    .arg(informat::diagnoseDeleteFailure(all)));
        }
    });
    dlg.exec();
}

// —— 开机自启动（用户级 HKCU Run，无需管理员）——
static const char* kAutoStartRegKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const char* kAutoStartValue  = "UninstallerManager";

bool UninstallerWindow::isAutoStart() const {
    QSettings run(QString::fromLatin1(kAutoStartRegKey), QSettings::NativeFormat);
    return !run.value(QString::fromLatin1(kAutoStartValue)).toString().isEmpty();
}

void UninstallerWindow::setAutoStart(bool on) {
    QSettings run(QString::fromLatin1(kAutoStartRegKey), QSettings::NativeFormat);
    const QString name = QString::fromLatin1(kAutoStartValue);
    if (on) {
        QString exe = QApplication::applicationFilePath();
        if (exe.isEmpty()) return;
        run.setValue(name, "\"" + exe + "\""); // 路径带引号，兼容含空格
    } else {
        run.remove(name);
    }
    run.sync(); // 立即落盘（退出走 ExitProcess 不保证析构写入）
    if (m_autostartAct) m_autostartAct->setChecked(isAutoStart());
    logOperation(QString::fromUtf8(u8"开机自启动"), QString::fromUtf8(u8"(设置)"), on,
                 on ? QString::fromUtf8(u8"已开启") : QString::fromUtf8(u8"已关闭"));
}

// 查看操作历史：读取 op_history.log（JSONL）并表格展示，支持导出 txt/csv。
void UninstallerWindow::showHistoryDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle(QString::fromUtf8(u8"操作历史"));
    dlg.resize(720, 460);
    QVBoxLayout* lay = new QVBoxLayout(&dlg);

    QTableWidget* tbl = new QTableWidget(&dlg);
    tbl->setColumnCount(5);
    tbl->setHorizontalHeaderLabels(QStringList{
        QString::fromUtf8(u8"时间"), QString::fromUtf8(u8"操作"),
        QString::fromUtf8(u8"软件"), QString::fromUtf8(u8"结果"),
        QString::fromUtf8(u8"详情") });
    tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
    tbl->setAlternatingRowColors(true);
    tbl->horizontalHeader()->setStretchLastSection(true);
    lay->addWidget(tbl);

    // 解析日志
    struct Rec { QString ts, op, name, ok, detail; };
    QVector<Rec> recs;
    QFile f(opLogPath());
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream ts(&f);
        while (!ts.atEnd()) {
            const QByteArray line = ts.readLine().toUtf8();
            if (line.trimmed().isEmpty()) continue;
            const QJsonObject o = QJsonDocument::fromJson(line).object();
            if (o.isEmpty()) continue;
            recs.append({ o.value("ts").toString(), o.value("op").toString(),
                          o.value("name").toString(),
                          o.value("success").toBool() ? QString::fromUtf8(u8"成功") : QString::fromUtf8(u8"失败"),
                          o.value("detail").toString() });
        }
    }
    // 倒序：最新在上
    tbl->setRowCount(recs.size());
    for (int i = 0; i < recs.size(); ++i) {
        const Rec& r = recs[recs.size() - 1 - i];
        tbl->setItem(i, 0, new QTableWidgetItem(r.ts));
        tbl->setItem(i, 1, new QTableWidgetItem(r.op));
        tbl->setItem(i, 2, new QTableWidgetItem(r.name));
        QTableWidgetItem* okIt = new QTableWidgetItem(r.ok);
        okIt->setForeground(r.ok == QString::fromUtf8(u8"成功")
                            ? QColor(46, 160, 67) : QColor(200, 60, 60));
        tbl->setItem(i, 3, okIt);
        tbl->setItem(i, 4, new QTableWidgetItem(r.detail));
    }
    tbl->resizeColumnsToContents();

    QHBoxLayout* btn = new QHBoxLayout();
    QPushButton* exportBtn = new QPushButton(QString::fromUtf8(u8"导出…"), &dlg);
    QPushButton* closeBtn = new QPushButton(QString::fromUtf8(u8"关闭"), &dlg);
    btn->addWidget(exportBtn);
    btn->addStretch(1);
    btn->addWidget(closeBtn);
    lay->addLayout(btn);

    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(exportBtn, &QPushButton::clicked, [&]() {
        if (recs.isEmpty()) { QMessageBox::information(&dlg, QString::fromUtf8(u8"导出"), QString::fromUtf8(u8"暂无操作记录。")); return; }
        const QString path = QFileDialog::getSaveFileName(&dlg, QString::fromUtf8(u8"导出操作历史"),
                                                          QStringLiteral("op_history.csv"),
                                                          QString::fromUtf8(u8"CSV 文件 (*.csv);;文本文件 (*.txt)"));
        if (path.isEmpty()) return;
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(&dlg, QString::fromUtf8(u8"导出失败"), QString::fromUtf8(u8"无法写入文件。"));
            return;
        }
        QTextStream ts(&out);
        const bool csv = path.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive);
        if (csv) {
            // UTF-8 BOM 防 Excel 乱码
            ts.setEncoding(QStringConverter::Utf8);
            ts << QChar(0xFEFF);   // 写 U+FEFF 经 UTF-8 编码即输出 EF BB BF BOM
            ts << QString::fromUtf8(u8"时间,操作,软件,结果,详情\n");
        }
        for (const Rec& r : recs) {
            if (csv) {
                auto esc = [](const QString& s) {
                    QString t = s; t.replace('"', "\"\"");
                    return QString("\"%1\"").arg(t);
                };
                ts << esc(r.ts) << ',' << esc(r.op) << ',' << esc(r.name) << ','
                   << esc(r.ok) << ',' << esc(r.detail) << '\n';
            } else {
                ts << QString::fromUtf8(u8"[%1] %2 %3 %4 — %5\n")
                      .arg(r.ts, r.op, r.name, r.ok, r.detail);
            }
        }
        QMessageBox::information(&dlg, QString::fromUtf8(u8"导出完成"),
                                 QString::fromUtf8(u8"已保存：%1").arg(path));
    });

    dlg.exec();
}

// 启动时的更新日志弹窗：一打开主界面就展示当前版本的更新内容。
// 从 CHANGELOG.md 解析“最新版本”段落（第一个 `## ` 标题下、到下一个 `## `/
// `---`/文件尾之间的内容），去掉 Markdown 引用与标题符号后返回纯文本。
// 文件缺失或解析失败时回退到内置文案，保证弹窗永远有内容可显示。
QString UninstallerWindow::loadChangelogLatest() {
    const QString fallback = QString::fromUtf8(
        u8"• ① 新增「运行中」分组：真实检测软件进程状态\n"
        u8"• ② 列表与体积扫描改为多核并行（QtConcurrent），扫描更快不卡顿\n"
        u8"• ③ 修复单字母搜索在排序模式下失效的问题\n"
        u8"• ④ 修复中文软件（如微信）被误判为残留\n"
        u8"• ⑤ 修复带空格路径「打开文件位置」打不开\n"
        u8"• ⑥ 安装位置自动校正（如 WeChat→Weixin 等过时路径）\n"
        u8"• ⑦ 系统组件默认隐藏，可在视图菜单切换显示\n"
        u8"• ⑧ 关闭窗口彻底退出，不再残留进程");

    QFile f(QCoreApplication::applicationDirPath() + QStringLiteral("/CHANGELOG.md"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return fallback;
    const QStringList lines = QString::fromUtf8(f.readAll()).split(QChar('\n'), Qt::KeepEmptyParts);

    // 定位第一个 `## ` 标题行（最新版本段起点）
    int start = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].startsWith(QStringLiteral("## "))) { start = i; break; }
    }
    if (start < 0) return fallback;

    QString out;
    for (int i = start + 1; i < lines.size(); ++i) {
        const QString raw = lines[i];
        // 遇到下一个版本标题或分隔线即停止
        if (raw.startsWith(QStringLiteral("## ")) || raw.startsWith(QStringLiteral("---"))) break;
        QString s = raw.trimmed();
        if (s.isEmpty()) { if (!out.isEmpty()) out += QChar('\n'); continue; }
        // 去掉 Markdown 引用前缀与标题符号，保留可读文本
        if (s.startsWith(QLatin1Char('>'))) s = s.mid(1).trimmed();
        if (s.startsWith(QStringLiteral("### "))) s = s.mid(4).trimmed();
        if (s.startsWith(QLatin1Char('#'))) s = s.mid(1).trimmed();
        // 列表项 "- " 换成圆点，更符合弹窗内纯文本排版
        if (s.startsWith(QStringLiteral("- "))) s = QStringLiteral("• ") + s.mid(2);
        out += s + QChar('\n');
    }
    out = out.trimmed();
    // 剥掉行内 Markdown 修饰符：粗体星号与行内代码反引号，纯文本里只会显得杂乱
    out.remove(QLatin1Char('*'));
    out.remove(QLatin1Char('`'));
    return out.isEmpty() ? fallback : out;
}

// 用户勾选“不再提示此版本”后，该版本不再弹出（基于 QSettings 持久化到注册表）。
// 必须用与全局一致的 QSettings("Uninstaller","uninstaller")：默认构造的 QSettings
// 组织名为空，写入位置与 loadLanguageSetting 等不一致，“不再提示”会失效（仍弹）。
void UninstallerWindow::showUpdatePopup() {
    QSettings settings(QStringLiteral("Uninstaller"), QStringLiteral("uninstaller"));
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
    m_updateDlg = &dlg;       // 供版本检查回调在弹窗仍打开时追加“新版本”提示
    m_updateLayout = layout;

    QLabel* title = new QLabel(QString::fromUtf8(u8"卸载管理器 %1").arg(appVersionFull()), &dlg);
    title->setStyleSheet("font-size:18px; font-weight:bold; color:#1a1a1a;");
    layout->addWidget(title);

    QLabel* sub = new QLabel(getlang(0x51).toString(), &dlg);
    sub->setStyleSheet("color:#555555;");
    layout->addWidget(sub);

    QTextEdit* te = new QTextEdit(&dlg);
    te->setReadOnly(true);
    te->setPlainText(loadChangelogLatest());
    // setPlainText 可能将光标置于文末导致内容滚动到中部，强制回到顶部展示
    te->moveCursor(QTextCursor::Start);
    te->ensureCursorVisible();
    te->setFixedHeight(230);
    layout->addWidget(te);

    QCheckBox* cb = new QCheckBox(getlang(0x52).toString(), &dlg);
    layout->addWidget(cb);

    QPushButton* ok = new QPushButton(getlang(0x53).toString(), &dlg);
    ok->setDefault(true);
    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    btnRow->addWidget(ok);
    layout->addLayout(btnRow);

    // 辅助：把弹窗诊断信息追加到 startup.log（便于排查“勾了仍弹”）
    auto logPopup = [&](const QString& msg) {
        appendStartupLog(QDateTime::currentDateTime().toString(Qt::ISODate)
                         + QStringLiteral(" [updatePopup] ") + msg);
    };

    QObject::connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
    // 勾选/取消时立即持久化，不再仅依赖 OK 路径；避免某些情况下 accept 未触发或设置未 flush
    QObject::connect(cb, &QCheckBox::checkStateChanged, [&](Qt::CheckState state) {
        if (state == Qt::Checked) {
            settings.setValue(dontShowKey, appVersionFull());
            logPopup(QStringLiteral("checked -> set lastShown=") + appVersionFull());
        } else {
            settings.remove(dontShowKey);
            logPopup(QStringLiteral("unchecked -> removed lastShown"));
        }
        settings.sync();
    });
    logPopup(QStringLiteral("read lastShown=") + last + QStringLiteral(" expected=") + appVersionFull()
             + QStringLiteral(" match=") + (last == appVersionFull() ? QStringLiteral("yes") : QStringLiteral("no")));

    int rc = dlg.exec();
    m_updateDlg = nullptr;    // 弹窗已关闭，后续到达的版本检查结果不再写入此弹窗
    m_updateLayout = nullptr;
    if (rc == QDialog::Accepted && cb->isChecked()) {
        settings.setValue(dontShowKey, appVersionFull());
        settings.sync();
        logPopup(QStringLiteral("accepted -> set lastShown=") + appVersionFull());
    } else if (rc == QDialog::Accepted && !cb->isChecked()) {
        logPopup(QStringLiteral("accepted -> unchecked, nothing written"));
    } else {
        logPopup(QStringLiteral("rejected rc=") + QString::number(rc));
    }
}

// 启动后异步拉取服务器版本信息。网络请求非阻塞（8 秒超时），
// 结果由 onVersionReplyFinished 回调处理；弹窗打开期间若检测到新版本会就地追加提示横幅，
// 不会拖慢主界面启动。
void UninstallerWindow::checkForUpdate() {
    if (!m_netMgr) return;
    QNetworkRequest req(QUrl(QString::fromUtf8(kUpdateCheckUrl)));
    req.setTransferTimeout(8000);
    m_netMgr->get(req);
}

// 把 "1.2.3" / "v1.2.3" 解析为按点分隔的整数列表，便于比较
static QList<int> parseVersion(const QString& v) {
    QList<int> out;
    QString body = v.trimmed();
    if (!body.isEmpty() && (body[0] == QLatin1Char('v') || body[0] == QLatin1Char('V')))
        body = body.mid(1);
    for (const QString& part : body.split(QLatin1Char('.'), Qt::SkipEmptyParts)) {
        bool ok = false;
        int n = part.toInt(&ok);
        out.append(ok ? n : 0);
    }
    return out;
}

// 返回 true 表示 remote 比本地 APP_VERSION 更新
static bool versionIsNewer(const QString& remote) {
    QList<int> a = parseVersion(remote);
    QList<int> b = parseVersion(QStringLiteral(APP_VERSION));
    const int n = qMax(a.size(), b.size());
    for (int i = 0; i < n; ++i) {
        const int x = (i < a.size()) ? a[i] : 0;
        const int y = (i < b.size()) ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

void UninstallerWindow::onVersionReplyFinished(QNetworkReply* reply) {
    if (!reply) return;
    const bool ok = (reply->error() == QNetworkReply::NoError);
    const QString errStr = reply->errorString();
    const QByteArray data = ok ? reply->readAll() : QByteArray();
    reply->deleteLater();
    if (!ok) {
        appendStartupLog(QDateTime::currentDateTime().toString(Qt::ISODate)
                         + QStringLiteral(" [updateCheck] network error=") + errStr);
        return;
    }

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        appendStartupLog(QDateTime::currentDateTime().toString(Qt::ISODate)
                         + QStringLiteral(" [updateCheck] json parse error"));
        return;
    }
    const QJsonObject obj = doc.object();
    const QString latest = obj.value(QStringLiteral("latest_version")).toString();
    if (latest.isEmpty()) return;

    // 仅在发现更新版本、且更新弹窗仍打开时，追加提示横幅 + 下载按钮
    if (versionIsNewer(latest) && m_updateDlg && m_updateLayout) {
        const QString dl = obj.value(QStringLiteral("download_url")).toString();
        const QString note = obj.value(QStringLiteral("changelog")).toString();

        QWidget* banner = new QWidget(m_updateDlg);
        banner->setStyleSheet(
            "QWidget{background-color:#eaf1ff;border:1px solid #b9d0ff;border-radius:6px;}"
            "QLabel{color:#10316b;}");
        QVBoxLayout* bl = new QVBoxLayout(banner);
        bl->setContentsMargins(12, 10, 12, 10);
        QLabel* title = new QLabel(
            QString::fromUtf8(u8"发现新版本 %1（当前 %2）").arg(latest).arg(appVersionFull()), banner);
        title->setStyleSheet("font-weight:bold;font-size:14px;color:#10316b;");
        bl->addWidget(title);
        if (!note.isEmpty()) {
            QLabel* body = new QLabel(note, banner);
            body->setWordWrap(true);
            bl->addWidget(body);
        }
        if (!dl.isEmpty()) {
            QPushButton* dlBtn = new QPushButton(QString::fromUtf8(u8"前往下载"), banner);
            dlBtn->setStyleSheet(
                "QPushButton{background-color:#3a6df0;color:#fff;border:none;border-radius:6px;"
                "padding:6px 16px;font-size:13px;} QPushButton:hover{background-color:#4f7ef5;}");
            connect(dlBtn, &QPushButton::clicked, banner, [dl]() {
                QDesktopServices::openUrl(QUrl(dl));
            });
            bl->addWidget(dlBtn);
        }
        m_updateLayout->insertWidget(m_updateLayout->count() - 1, banner); // 插到“不再提示”复选框前
        appendStartupLog(QDateTime::currentDateTime().toString(Qt::ISODate)
                         + QStringLiteral(" [updateCheck] newer version shown: ") + latest);
    } else {
        appendStartupLog(QDateTime::currentDateTime().toString(Qt::ISODate)
                         + QStringLiteral(" [updateCheck] up to date (latest=") + latest + QStringLiteral(")"));
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
    QString statusStr = sw->isOrphaned ? (getlang(0x49).toString() + QString::fromUtf8(u8"（卸载程序不存在）"))
                                        : getlang(0x4B).toString();
    form->addRow(getlang(0x42).toString(), new QLabel(statusStr));
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
    // 仅残留项（已卸载但仍有遗留文件）才有可扫描的残留；正常软件置灰并给出原因，
    // 避免用户点击后只看到模糊的“未找到”。
    if (!sw->isOrphaned) {
        btnScan->setEnabled(false);
        btnScan->setToolTip(QString::fromUtf8(u8"仅“残留项”（已卸载但仍有遗留文件）可扫描残留"));
    }
    QPushButton* btnCopy = new QPushButton(getlang(0x2F).toString());
    funcGrid->addWidget(btnOpen, 0, 0);
    funcGrid->addWidget(btnUninstall, 0, 1);
    funcGrid->addWidget(btnScan, 1, 0);
    funcGrid->addWidget(btnCopy, 1, 1);
    QPushButton* btnDelReg = new QPushButton(getlang(0x4E).toString());
    btnDelReg->setObjectName("uninstallBtn");
    funcGrid->addWidget(btnDelReg, 2, 0, 1, 2);
    QPushButton* btnForceDel = new QPushButton(getlang(0x4F).toString());
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
    // 防御：target 若含双引号（注册表被构造的异常值）会破坏 /select 参数解析，
    // 直接剔除（Windows 文件名本就不允许双引号，正常路径不会含）。
    QString native = QDir::toNativeSeparators(target).replace("\"", "");
    QString params;
    if (selectFile) {
        params = QString("/select,\"%1\"").arg(native);
    } else {
        params = QString("/e,\"%1\"").arg(native);
    }
    // 资源管理器实际位于 %SystemRoot%（C:\Windows\explorer.exe），并不在 System32 下。
    // 用 GetWindowsDirectoryW 取 Windows 主目录完整路径：既修复“打开文件位置”失效，
    // 又避免从应用目录解析 explorer.exe（F4 防 DLL/二进制种植）。
    wchar_t winDir[MAX_PATH] = {0};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    std::wstring explorer = (winDir[0] ? std::wstring(winDir) : std::wstring(L"C:\\Windows"));
    if (!explorer.empty() && explorer.back() != L'\\') explorer += L'\\';
    explorer += L"explorer.exe";
    auto ret = reinterpret_cast<intptr_t>(::ShellExecuteW(
        nullptr, L"open", explorer.c_str(),
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

// 将已收集的软件行写入文件（CSV/HTML/TSV），返回是否成功。导出全部与导出选中共用。
static bool writeExportRows(const QVector<ExportRow>& rows, const QString& fileName) {
    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    QString lower = fileName.toLower();
    if (lower.endsWith(".html")) {
        out << "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
               "<title>软件清单</title><style>"
               "table{border-collapse:collapse;font-family:sans-serif;font-size:13px}"
               "th,td{border:1px solid #ccc;padding:4px 8px;text-align:left}"
               "th{background:#f0f0f0}tr:nth-child(even){background:#fafafa}</style></head><body>"
               "<h2>已安装软件清单</h2><table><thead><tr>"
               "<th>名称</th><th>版本</th><th>发行商</th><th>安装日期</th><th>大小</th><th>安装位置</th><th>状态</th><th>卸载命令</th>"
               "</tr></thead><tbody>\n";
        for (const auto& r : rows) {
            auto esc = [](const QString& s) { return s.toHtmlEscaped(); };
            out << "<tr><td>" << esc(r.name) << "</td><td>" << esc(r.ver) << "</td><td>"
                << esc(r.pub) << "</td><td>" << esc(r.date) << "</td><td>" << esc(r.size)
                << "</td><td>" << esc(r.loc) << "</td><td>" << esc(r.status) << "</td><td>"
                << esc(r.cmd) << "</td></tr>\n";
        }
        out << "</tbody></table></body></html>";
    } else if (lower.endsWith(".csv")) {
        out.setGenerateByteOrderMark(true);
        auto csvCell = [](const QString& s) -> QString {
            QString cell = s;
            if (!cell.isEmpty() && (cell.at(0) == QChar('=') || cell.at(0) == QChar('+') ||
                                     cell.at(0) == QChar('-') || cell.at(0) == QChar('@')))
                cell.prepend(QChar('\''));
            if (cell.contains(',') || cell.contains('"') || cell.contains('\n') || cell.contains('\r')) {
                cell.replace('"', "\"\"");
                return "\"" + cell + "\"";
            }
            return cell;
        };
        out << "Name,Version,Publisher,InstallDate,Size,Location,Status,UninstallCommand\n";
        for (const auto& r : rows) {
            out << csvCell(r.name) << "," << csvCell(r.ver) << "," << csvCell(r.pub) << ","
                << csvCell(r.date) << "," << csvCell(r.size) << "," << csvCell(r.loc) << ","
                << csvCell(r.status) << "," << csvCell(r.cmd) << "\n";
        }
    } else {
        out.setGenerateByteOrderMark(true);
        auto tsvCell = [](const QString& s) -> QString {
            if (!s.isEmpty() && (s.at(0) == QChar('=') || s.at(0) == QChar('+') ||
                                 s.at(0) == QChar('-') || s.at(0) == QChar('@')))
                return QString(QChar('\'')) + s;
            return s;
        };
        out << "Name\tVersion\tPublisher\tInstallDate\tSize\tLocation\tStatus\tUninstallCommand\n";
        for (const auto& r : rows) {
            out << tsvCell(r.name) << "\t" << tsvCell(r.ver) << "\t" << tsvCell(r.pub) << "\t" << tsvCell(r.date) << "\t"
                << tsvCell(r.size) << "\t" << tsvCell(r.loc) << "\t" << tsvCell(r.status) << "\t" << tsvCell(r.cmd) << "\n";
        }
    }
    file.close();
    return true;
}

void UninstallerWindow::exportSoftwareList() {
    QSettings exp(QStringLiteral("Uninstaller"), QStringLiteral("uninstaller"));
    QString lastDir = exp.value(QStringLiteral("lastExportDir"), QCoreApplication::applicationDirPath()).toString();
    QString fileName = QFileDialog::getSaveFileName(
        this,
        getlang(0x2E).toString(),
        lastDir + QStringLiteral("/software_list.csv"),
        QString::fromUtf8(u8"CSV 文件 (*.csv);;HTML 文件 (*.html);;文本文件 (*.txt);;所有文件 (*)"));
    if (fileName.isEmpty()) return;
    exp.setValue(QStringLiteral("lastExportDir"), QFileInfo(fileName).absolutePath());

    // 收集当前可见行数据（导出全部）
    QVector<ExportRow> rows = collectExportRows(false);

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
               "<th>名称</th><th>版本</th><th>发行商</th><th>安装日期</th><th>大小</th><th>安装位置</th><th>状态</th><th>卸载命令</th>"
               "</tr></thead><tbody>\n";
        for (const auto& r : rows) {
            // 仅用 toHtmlEscaped() 做转义；不要再手工替换 & < >，否则会与
            // toHtmlEscaped 内部转义叠加造成双重转义（浏览器里 & 会变成字面 &amp;）。
            auto esc = [](const QString& s) { return s.toHtmlEscaped(); };
            out << "<tr><td>" << esc(r.name) << "</td><td>" << esc(r.ver) << "</td><td>"
                << esc(r.pub) << "</td><td>" << esc(r.date) << "</td><td>" << esc(r.size)
                << "</td><td>" << esc(r.loc) << "</td><td>" << esc(r.status) << "</td><td>"
                << esc(r.cmd) << "</td></tr>\n";
        }
        out << "</tbody></table></body></html>";
    } else if (lower.endsWith(".csv")) {
        // 写 UTF-8 BOM，使 Excel 能正确识别中文（否则默认按 ANSI 解析会乱码）。
        out.setGenerateByteOrderMark(true);
        // CSV：字段含逗号/引号/换行时按 RFC4180 用双引号包裹并转义内部引号。
        // CWE-1236 公式注入防护：DisplayVersion/DisplayName 等来自注册表（任何安装程序可任意写入），
        // 前导 = + - @ 会被表格软件误判为公式执行；前置单引号 ' 强制视为纯文本。
        auto csvCell = [](const QString& s) -> QString {
            QString cell = s;
            if (!cell.isEmpty() && (cell.at(0) == QChar('=') || cell.at(0) == QChar('+') ||
                                     cell.at(0) == QChar('-') || cell.at(0) == QChar('@')))
                cell.prepend(QChar('\''));
            if (cell.contains(',') || cell.contains('"') || cell.contains('\n') || cell.contains('\r')) {
                cell.replace('"', "\"\"");
                return "\"" + cell + "\"";
            }
            return cell;
        };
        out << "Name,Version,Publisher,InstallDate,Size,Location,Status,UninstallCommand\n";
        for (const auto& r : rows) {
            out << csvCell(r.name) << "," << csvCell(r.ver) << "," << csvCell(r.pub) << ","
                << csvCell(r.date) << "," << csvCell(r.size) << "," << csvCell(r.loc) << ","
                << csvCell(r.status) << "," << csvCell(r.cmd) << "\n";
        }
    } else {
        // 文本/TSV 也写 UTF-8 BOM，避免 Excel 打开中文乱码。
        out.setGenerateByteOrderMark(true);
        // TSV 同样存在公式注入（Excel 对制表符分隔也按单元格解析），前导 = + - @ 同样前置单引号。
        auto tsvCell = [](const QString& s) -> QString {
            if (!s.isEmpty() && (s.at(0) == QChar('=') || s.at(0) == QChar('+') ||
                                 s.at(0) == QChar('-') || s.at(0) == QChar('@')))
                return QString(QChar('\'')) + s;
            return s;
        };
        out << "Name\tVersion\tPublisher\tInstallDate\tSize\tLocation\tStatus\tUninstallCommand\n";
        for (const auto& r : rows) {
            out << tsvCell(r.name) << "\t" << tsvCell(r.ver) << "\t" << tsvCell(r.pub) << "\t" << tsvCell(r.date) << "\t"
                << tsvCell(r.size) << "\t" << tsvCell(r.loc) << "\t" << tsvCell(r.status) << "\t" << tsvCell(r.cmd) << "\n";
        }
    }
    file.close();
    QMessageBox::information(this, getlang(0x31).toString(), getlang(0x32).toString().arg(fileName));
}

// 收集导出行：selectedOnly=true 时仅含当前选中行，否则含全部可见行。
QVector<ExportRow> UninstallerWindow::collectExportRows(bool selectedOnly) const {
    QVector<ExportRow> rows;
    QVector<int> selRows;
    if (selectedOnly && m_tableWidget && m_tableWidget->selectionModel()) {
        for (const QModelIndex& idx : m_tableWidget->selectionModel()->selectedRows())
            selRows.append(idx.row());
    }
    for (int i = 0; i < m_tableWidget->rowCount(); ++i) {
        if (m_tableWidget->isRowHidden(i)) continue;
        if (selectedOnly && !selRows.contains(i)) continue;
        auto sw = softwareAtRow(i);
        if (!sw) continue;
        rows.append({ QString::fromStdString(sw->displayName),
                      QString::fromStdString(sw->displayVersion),
                      QString::fromStdString(sw->publisher),
                      QString::fromStdString(sw->installDate),
                      QString::fromStdString(sw->size.get()),
                      QString::fromStdString(sw->installLocation),
                      sw->isOrphaned ? QString::fromUtf8(u8"残留")
                                     : (sw->isRunningTime ? QString::fromUtf8(u8"运行中")
                                                          : QString::fromUtf8(u8"正常")),
                      QString::fromStdString(Registry::getUninstallCommand(*sw)) });
    }
    return rows;
}

void UninstallerWindow::exportSelectedSoftware() {
    QItemSelectionModel* sel = m_tableWidget ? m_tableWidget->selectionModel() : nullptr;
    if (!sel || sel->selectedRows().isEmpty()) {
        QMessageBox::information(this, QString::fromUtf8(u8"导出选中软件"),
            QString::fromUtf8(u8"请先在列表中选中要导出的软件（按住 Ctrl / Shift 可多选）。"));
        return;
    }
    QSettings exp(QStringLiteral("Uninstaller"), QStringLiteral("uninstaller"));
    QString lastDir = exp.value(QStringLiteral("lastExportDir"), QCoreApplication::applicationDirPath()).toString();
    QString fileName = QFileDialog::getSaveFileName(
        this, QString::fromUtf8(u8"导出选中软件"),
        lastDir + QStringLiteral("/selected_software.csv"),
        QString::fromUtf8(u8"CSV 文件 (*.csv);;HTML 文件 (*.html);;文本文件 (*.txt);;所有文件 (*)"));
    if (fileName.isEmpty()) return;
    exp.setValue(QStringLiteral("lastExportDir"), QFileInfo(fileName).absolutePath());
    QVector<ExportRow> rows = collectExportRows(true);
    if (writeExportRows(rows, fileName))
        QMessageBox::information(this, getlang(0x31).toString(), getlang(0x32).toString().arg(fileName));
    else
        QMessageBox::warning(this, getlang(0x2Eu).toString(), QString::fromUtf8(u8"无法导出文件，请检查保存路径与写入权限。"));
}

void UninstallerWindow::copyUninstallCommand() {
    int row = m_tableWidget->currentRow();
    if (tick(row)) return;

    auto sw = softwareAtRow(row);
    // 复制“规范化后可运行”的卸载命令（MSI 会补全为 `msiexec.exe /x {GUID} /quiet /norestart`，
    // 普通程序用原始字符串），比原始 UninstallString 更便于直接粘贴到命令行/脚本执行。
    QString cmd = QString::fromStdString(Registry::getUninstallCommand(*sw));
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
    {QString::fromUtf8(u8"网易云音乐"), QStringLiteral("netease"), QStringLiteral("cloudmusic")},
    {QString::fromUtf8(u8"腾讯会议"), QStringLiteral("wemeet"), QStringLiteral("tencentmeeting")},
    {QString::fromUtf8(u8"企业微信"), QStringLiteral("wxwork"), QStringLiteral("wecom")},
    {QString::fromUtf8(u8"百度网盘"), QStringLiteral("baidunetdisk"), QStringLiteral("baiduyun")},
    {QString::fromUtf8(u8"QQ音乐"), QStringLiteral("qqmusic"), QStringLiteral("qqyy")},
    {QString::fromUtf8(u8"飞书"), QStringLiteral("feishu"), QStringLiteral("lark")},
    {QString::fromUtf8(u8"原神"), QStringLiteral("genshin")},
    {QString::fromUtf8(u8"米哈游"), QStringLiteral("mihoyo"), QStringLiteral("miHoYo")},
    {QString::fromUtf8(u8"喜马拉雅"), QStringLiteral("ximalaya")},
    {QString::fromUtf8(u8"知乎"), QStringLiteral("zhihu")},
    {QString::fromUtf8(u8"微博"), QStringLiteral("weibo")},
    {QString::fromUtf8(u8"小红书"), QStringLiteral("xhs"), QStringLiteral("redbook")},
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

    // 名称列搜索高亮：把原始输入分词交给委托（原始大小写，直接命中显示名）
    if (m_hlDelegate) {
        m_hlTokens = tokens;
        m_hlDelegate->setTokens(tokens);
        m_tableWidget->viewport()->update();
    }

    // setRowHidden() 在排序启用时与排序代理的 visual/logical 行映射交互不可靠，
    // 会表现为隐藏/显示错乱、过滤不生效。因此：搜索词非空时彻底关闭排序；
    // 清空搜索词后再恢复排序。这样隐藏状态在稳定环境下工作。
    // 恢复排序时保留用户此前设置的排序列/方向（而非强制回到名称升序），
    // 否则用户搜完清空搜索框后，刚按大小排好的列表会被重置，体验差。
    // 仅当有搜索词“且”未开启“仅显示残留项”时才启用排序。开启残留过滤时排序同样必须
    // 关闭：否则 setRowHidden 在排序启用下与排序代理的行映射交互不可靠（见上方注释），
    // 会导致残留过滤隐藏/显示错乱、过滤不生效。与搜索词路径保持一致。
    bool wantSorting = compactTokens.isEmpty() && !m_showOrphanOnly && m_statusFilter == 0;
    if (m_tableWidget->isSortingEnabled() != wantSorting) {
        int sortCol = -1;
        Qt::SortOrder sortOrder = Qt::AscendingOrder;
        if (!wantSorting) {
            // 即将关闭排序：先记住当前的排序列/方向，供清空搜索时恢复。
            sortCol = m_tableWidget->horizontalHeader()->sortIndicatorSection();
            sortOrder = m_tableWidget->horizontalHeader()->sortIndicatorOrder();
        }
        m_tableWidget->setSortingEnabled(wantSorting);
        if (wantSorting) {
            if (sortCol >= 0) {
                m_tableWidget->sortItems(sortCol, sortOrder);
            } else {
                m_tableWidget->sortItems(0, Qt::AscendingOrder);
            }
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
            // 状态筛选：1 正常 / 2 运行中 / 3 残留（0 全部）
            bool statusOk = true;
            if (m_statusFilter == 1) statusOk = (!sw->isOrphaned && !sw->isRunningTime);
            else if (m_statusFilter == 2) statusOk = sw->isRunningTime;
            else if (m_statusFilter == 3) statusOk = sw->isOrphaned;
            match = nameOk && orphanOk && statusOk;
        }
        m_tableWidget->setRowHidden(i, !match);
        if (match) {
            ++visibleCount;
            if (sw) visibleSize += sw->size.size;
        }
    }

    // 状态栏：根据是否有搜索词/残留过滤，给出更明确的可视反馈。
    // 0 匹配时尤其要提示“是过滤导致全部隐藏”而非“列表空了”，避免误以为扫描失败。
    QString statusMsg;
    if (visibleCount == 0 && !compactTokens.isEmpty()) {
        statusMsg = QString::fromUtf8(u8"未找到与「%1」匹配的项（共 %2 个软件，已全部被过滤隐藏）")
            .arg(raw).arg(m_softwareList.size());
    } else if (visibleCount == 0 && m_showOrphanOnly) {
        statusMsg = QString::fromUtf8(u8"没有残留项（共 %1 个软件）").arg(m_softwareList.size());
    } else if (visibleCount == 0 && m_statusFilter != 0) {
        statusMsg = QString::fromUtf8(u8"没有符合该状态的项（共 %1 个软件）").arg(m_softwareList.size());
    } else {
        statusMsg = getlang(0x7u).toString()
            .arg(visibleCount).arg(QString::fromStdString(visibleSize.get()));
    }
    // 追加选中信息（若有选中）：方便批量卸载/删残留前确认范围
    int selCount = 0;
    filesize_t selSize;
    if (QItemSelectionModel* sel = m_tableWidget->selectionModel()) {
        for (const QModelIndex& idx : sel->selectedRows()) {
            ++selCount;
            auto sw = softwareAtRow(idx.row());
            if (sw) selSize += sw->size.size;
        }
    }
    if (selCount > 0) {
        statusMsg += QString::fromUtf8(u8"　已选 %1 个，共 %2")
            .arg(selCount).arg(QString::fromStdString(selSize.get()));
    }
    statusBar()->showMessage(statusMsg);
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
        QMenu* sub = m_langMenu->addMenu(langRegionName(primary, G.LANGUAGE));
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
        QMenu* other = m_langMenu->addMenu(langRegionName(QStringLiteral("其他"), G.LANGUAGE));
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
    // 视图菜单标题 + 亮/暗主题项随界面语言翻译
    if (m_viewMenu) m_viewMenu->setTitle(getlang(0x43).toString());
    if (m_lightThemeAct) m_lightThemeAct->setText(getlang(0x44).toString());
    if (m_darkThemeAct) m_darkThemeAct->setText(getlang(0x45).toString());
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
    if (m_orphanOnlyCheck) m_orphanOnlyCheck->setText(getlang(0x48).toString());

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
    if (m_batchUninstallBtn) m_batchUninstallBtn->setText(getlang(0x46).toString());
    if (m_batchDelBtn) m_batchDelBtn->setText(getlang(0x47).toString());

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
    actionMenu->addSeparator();
    // 选择辅助：仅作用于当前可见（未被过滤隐藏）的行
    actionMenu->addAction(QString::fromUtf8(u8"全选"), this, [this]() {
        for (int r = 0; r < m_tableWidget->rowCount(); ++r)
            if (!m_tableWidget->isRowHidden(r)) m_tableWidget->selectRow(r);
    });
    actionMenu->addAction(QString::fromUtf8(u8"反选"), this, [this]() {
        for (int r = 0; r < m_tableWidget->rowCount(); ++r) {
            if (m_tableWidget->isRowHidden(r)) continue;
            QTableWidgetItem* it = m_tableWidget->item(r, 0);
            if (it) it->setSelected(!it->isSelected());
        }
    });
    actionMenu->addAction(QString::fromUtf8(u8"清除选择"), this, [this]() {
        m_tableWidget->clearSelection();
    });
    actionMenu->addSeparator();
    // 全局残留体检：扫描所有残留项并批量清理（主动巡检）
    actionMenu->addAction(QString::fromUtf8(u8"扫描所有残留…"), this, &UninstallerWindow::scanAllResiduals);

    // “本程序”菜单：提供卸载自身的能力（区别于卸载列表中的其他软件）。
    m_selfMenu = bar->addMenu(getlang(0x3E).toString());
    m_selfUninstallAction = m_selfMenu->addAction(getlang(0x3F).toString(), this, &UninstallerWindow::uninstallSelf);
    m_selfMenu->addSeparator();
    m_aboutAction = m_selfMenu->addAction(getlang(0x40).toString(), this, &UninstallerWindow::showAbout);
    m_selfMenu->addSeparator();
    m_selfMenu->addAction(QString::fromUtf8(u8"查看操作历史…"), this, &UninstallerWindow::showHistoryDialog);
    m_selfMenu->addSeparator();
    // 开机自启动（用户级 HKCU Run，无需管理员）
    m_autostartAct = m_selfMenu->addAction(QString::fromUtf8(u8"开机自启动"));
    m_autostartAct->setCheckable(true);
    m_autostartAct->setChecked(isAutoStart());
    connect(m_autostartAct, &QAction::triggered, this, &UninstallerWindow::setAutoStart);

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
    m_viewMenu = bar->addMenu(getlang(0x43).toString());
    m_lightThemeAct = m_viewMenu->addAction(getlang(0x44).toString());
    m_darkThemeAct = m_viewMenu->addAction(getlang(0x45).toString());
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

    // 自定义背景：选择图片 / 浓度三档 / 恢复默认
    m_viewMenu->addSeparator();
    m_bgSetAct = m_viewMenu->addAction(QString::fromUtf8(u8"自定义背景图片…"),
                                       this, &UninstallerWindow::setCustomBackground);
    QMenu* dimMenu = m_viewMenu->addMenu(QString::fromUtf8(u8"背景浓度"));
    QActionGroup* dimGroup = new QActionGroup(this);
    dimGroup->setExclusive(true);
    const QString kDimNames[3] = {
        QString::fromUtf8(u8"淡（背景更明显）"),
        QString::fromUtf8(u8"中"),
        QString::fromUtf8(u8"浓（文字更清晰）")};
    for (int i = 0; i < 3; ++i) {
        m_bgDimActs[i] = dimMenu->addAction(kDimNames[i]);
        m_bgDimActs[i]->setCheckable(true);
        m_bgDimActs[i]->setActionGroup(dimGroup);
        connect(m_bgDimActs[i], &QAction::triggered, this, [this, i]() { setBgDim(i); });
    }

    // 背景模式：铺满(cover) / 居中(center) / 平铺(tile)
    QMenu* modeMenu = m_viewMenu->addMenu(QString::fromUtf8(u8"背景模式"));
    QActionGroup* modeGroup = new QActionGroup(this);
    modeGroup->setExclusive(true);
    const QString kModeNames[3] = {
        QString::fromUtf8(u8"铺满（裁剪留全图）"),
        QString::fromUtf8(u8"居中（原尺寸）"),
        QString::fromUtf8(u8"平铺（重复）")};
    for (int i = 0; i < 3; ++i) {
        m_bgModeActs[i] = modeMenu->addAction(kModeNames[i]);
        m_bgModeActs[i]->setCheckable(true);
        m_bgModeActs[i]->setActionGroup(modeGroup);
        connect(m_bgModeActs[i], &QAction::triggered, this, [this, i]() { setBgMode(i); });
    }

    // 背景模糊开关（铺满/居中更柔和；平铺不模糊）
    m_bgBlurAct = m_viewMenu->addAction(QString::fromUtf8(u8"背景模糊"));
    m_bgBlurAct->setCheckable(true);
    connect(m_bgBlurAct, &QAction::triggered, this, [this](bool on) { setBgBlur(on); });

    // 使用系统桌面壁纸作为背景（实时读取当前壁纸路径）
    m_bgDesktopAct = m_viewMenu->addAction(QString::fromUtf8(u8"使用系统桌面壁纸"),
                                           this, &UninstallerWindow::useDesktopWallpaper);

    m_bgClearAct = m_viewMenu->addAction(QString::fromUtf8(u8"恢复默认背景"),
                                         this, &UninstallerWindow::clearCustomBackground);
    syncBgMenuChecks();

    // 列显隐子菜单（视图 ▸ 显示列）：勾选控制各列可见性
    QMenu* colMenu = m_viewMenu->addMenu(QString::fromUtf8(u8"显示列"));    const QString kColNames[7] = {
        QString::fromUtf8(u8"名称"), QString::fromUtf8(u8"版本"), QString::fromUtf8(u8"发行商"),
        QString::fromUtf8(u8"大小"), QString::fromUtf8(u8"安装日期"), QString::fromUtf8(u8"安装位置"),
        QString::fromUtf8(u8"状态") };
    for (int i = 0; i < 7; ++i) {
        m_colActs[i] = colMenu->addAction(kColNames[i]);
        m_colActs[i]->setCheckable(true);
        m_colActs[i]->setChecked(true);
        connect(m_colActs[i], &QAction::toggled, this, [this, i](bool on) {
            if (m_tableWidget) m_tableWidget->setColumnHidden(i, !on);
        });
    }

    m_viewMenu->addSeparator();
    // 磁盘占用排行（Top15 条形图，点击跳转选中）
    m_viewMenu->addAction(QString::fromUtf8(u8"占用排行…"), this, &UninstallerWindow::showSizeRanking);

    // 系统托盘：关闭最小化到托盘
    createTray();

    // 工具栏
    QHBoxLayout* toolBar = new QHBoxLayout();
    toolBar->setSpacing(8);
    toolBar->setContentsMargins(0, 4, 0, 4);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(getlang(0x22).toString());
    m_searchEdit->setClearButtonEnabled(true);  // 右侧显示清除(×)按钮，一键清空搜索
    m_searchEdit->setMinimumWidth(220);
    m_searchEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &UninstallerWindow::filterSoftware);

    // Ctrl+F 快速聚焦搜索框
    QShortcut* searchShortcut = new QShortcut(QKeySequence("Ctrl+F"), this);
    connect(searchShortcut, &QShortcut::activated, this, [this]() {
        m_searchEdit->setFocus();
        m_searchEdit->selectAll();
    });

    // F5 强制重新扫描注册表（与刷新按钮一致，反映新装/卸载的软件）
    QShortcut* refreshShortcut = new QShortcut(QKeySequence("F5"), this);
    connect(refreshShortcut, &QShortcut::activated, this, [this]() {
        built_list(false);
        loadSoftwareList();
    });

    // Esc 清空搜索框（仅当搜索框聚焦时触发，避免与对话框 Esc 冲突）
    QShortcut* escShortcut = new QShortcut(QKeySequence("Esc"), m_searchEdit);
    connect(escShortcut, &QShortcut::activated, m_searchEdit, &QLineEdit::clear);

    // Ctrl+A 在表格聚焦时全选可见行（限定表格 Context，不干扰搜索框内 Ctrl+A 选词）
    QShortcut* selectAllShortcut = new QShortcut(QKeySequence("Ctrl+A"), m_tableWidget);
    connect(selectAllShortcut, &QShortcut::activated, this, [this]() {
        for (int r = 0; r < m_tableWidget->rowCount(); ++r)
            if (!m_tableWidget->isRowHidden(r)) m_tableWidget->selectRow(r);
    });

    m_refreshBtn = new QPushButton(getlang(0x23).toString(), this);
    m_refreshBtn->setObjectName("refreshBtn");
    connect(m_refreshBtn, &QPushButton::clicked, this, [this]() {
        built_list(false);  // 强制重新扫描注册表（反映新装/卸载的软件，不受缓存窗口影响）
        loadSoftwareList(); // 重新加载并套用当前搜索过滤
    });

    m_scanLabel = new QLabel(getlang(0x24).toString());
    toolBar->addWidget(m_scanLabel);
    // 搜索框随窗口宽度自动伸缩，占满中间空白
    toolBar->addWidget(m_searchEdit, /*stretch=*/1);
    toolBar->addWidget(m_refreshBtn);

    // 仅显示残留项过滤
    m_orphanOnlyCheck = new QCheckBox(getlang(0x48).toString(), this);
    connect(m_orphanOnlyCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_showOrphanOnly = on;
        filterSoftware();
    });
    toolBar->addWidget(m_orphanOnlyCheck);

    // 状态筛选下拉：全部 / 正常 / 运行中 / 残留（可叠加搜索词与残留开关）
    m_statusCombo = new QComboBox(this);
    m_statusCombo->addItems(QStringList{
        QString::fromUtf8(u8"全部状态"),
        QString::fromUtf8(u8"正常"),
        QString::fromUtf8(u8"运行中"),
        QString::fromUtf8(u8"残留") });
    m_statusCombo->setCurrentIndex(0);
    m_statusCombo->setToolTip(QString::fromUtf8(u8"按运行状态筛选列表"));
    connect(m_statusCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) { m_statusFilter = idx; filterSoftware(); });
    toolBar->addWidget(m_statusCombo);

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
    m_tableWidget->horizontalHeader()->setSectionsMovable(true); // 列可拖拽重排（顺序+宽度经 tableHeaderState 持久化）
    m_tableWidget->setSortingEnabled(true);
    // 名称列搜索高亮委托（叠加在基类绘制之上，不影响图标/选中态）
    m_hlDelegate = new HighlightDelegate(this);
    m_tableWidget->setItemDelegateForColumn(0, m_hlDelegate);

    // 右键菜单：卸载 / 扫描残留 / 详情 / 复制命令
    // 用事件过滤器直接在 viewport 上拦截右键，比 contextMenuPolicy 更可靠。
    m_tableWidget->viewport()->installEventFilter(this);

    // 双击行打开详情对话框（与右键「详情」一致），符合常见列表操作直觉
    connect(m_tableWidget, &QTableWidget::doubleClicked, this, &UninstallerWindow::showDetails);

    // 选择变化即刷新状态栏的「已选 N 个，共 X」提示（不含重新过滤）
    connect(m_tableWidget->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {
        int visible = 0; filesize_t vis;
        int sel = 0; filesize_t selSize;
        for (int r = 0; r < m_tableWidget->rowCount(); ++r) {
            auto sw = softwareAtRow(r);
            if (!m_tableWidget->isRowHidden(r)) {
                ++visible;
                if (sw) vis += sw->size.size;
            }
            QTableWidgetItem* it = m_tableWidget->item(r, 0);
            if (it && it->isSelected()) {
                ++sel;
                if (sw) selSize += sw->size.size;
            }
        }
        QString msg = getlang(0x7u).toString().arg(visible).arg(QString::fromStdString(vis.get()));
        if (sel > 0) {
            msg += QString::fromUtf8(u8"　已选 %1 个，共 %2")
                .arg(sel).arg(QString::fromStdString(selSize.get()));
        }
        statusBar()->showMessage(msg);
    });

    mainLayout->addWidget(m_tableWidget);

    // 按钮栏
    QHBoxLayout* buttonBar = new QHBoxLayout();

    m_batchUninstallBtn = new QPushButton(getlang(0x46).toString(), this);
    m_batchUninstallBtn->setObjectName("batchUninstallBtn");
    connect(m_batchUninstallBtn, &QPushButton::clicked, this, &UninstallerWindow::batchUninstall);
    m_batchDelBtn = new QPushButton(getlang(0x47).toString(), this);
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
