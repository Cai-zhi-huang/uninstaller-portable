#include <language.hpp>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCoreApplication>
#include <utility>

constexpr QStringView NONETEXT { u"" };

// 运行时语言表：[id][lang] -> 译文；与 JSON 的 entries 二维数组对应
static std::vector<QString>                  g_langNames;
static std::vector<QString>                  g_langFamilies;
static std::vector<std::vector<QString>>     g_table;
// 语言名翻译表：nameTable[display][target] = 用 display 语言写成的 target 语言名。
// 与 JSON 的 nameTable 二维数组对应；缺失时 langName(i, uiLang) 会回退到母语名称。
static std::vector<std::vector<QString>>     g_nameTable;
// 大区名翻译表：regionTable[原始大区名][uiLang] = 译文，来自 JSON 的 regions 对象。
static std::vector<std::pair<QString, std::vector<QString>>> g_regionTable;

void loadLanguageTable() {
    // 查找顺序：1) exe 同级目录的 lang/languages.json（可被外部文件覆盖，方便增删语言）
    //           2) 内嵌资源 :/lang/languages.json（编译时打包，保证始终可用）
    const QStringList candidates = {
        QCoreApplication::applicationDirPath() + QStringLiteral("/lang/languages.json"),
        QStringLiteral(":/lang/languages.json")
    };
    for (const QString& path : candidates) {
        QFile f(path);
        if (!f.exists() || !f.open(QIODevice::ReadOnly))
            continue;
        // DoS 护栏：外部 languages.json 体积异常大时直接跳过候选，
        // 避免 readAll 把超大文件整个读进内存（exe 同级目录可被攻击者写入）。
        // 16 MB 对 108+ 语言表绰绰有余，正常文件远小于此。
        constexpr qint64 kMaxLangJsonBytes = 16 * 1024 * 1024;
        if (f.size() > kMaxLangJsonBytes)
            continue;
        QJsonParseError parseErr;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
        if (doc.isNull())
            continue;
        const QJsonObject obj = doc.object();
        const QJsonArray langs    = obj.value(QLatin1String("languages")).toArray();
        const QJsonArray entries  = obj.value(QLatin1String("entries")).toArray();
        // families 是可选字段：缺失则全部回退到空串，由 UI 端视为“其他”组。
        const QJsonArray families = obj.value(QLatin1String("families")).toArray();
        // nameTable 是可选字段：缺失则留空，langName(i, uiLang) 回退到母语名称。
        const QJsonArray nameTable = obj.value(QLatin1String("nameTable")).toArray();

        std::vector<QString> names;
        names.reserve(langs.size());
        for (const QJsonValue& v : langs) names.push_back(v.toString());

        std::vector<QString> fams;
        fams.reserve(langs.size());
        // families 与 languages 位置一一对应；缺失项用空串占位。
        // 若 families 整体为空（老 JSON / 字段被裁剪），则全部为空串。
        for (int i = 0; i < names.size(); ++i) {
            if (i < families.size()) fams.push_back(families.at(i).toString());
            else                     fams.push_back(QString());
        }

        std::vector<std::vector<QString>> table;
        table.reserve(entries.size());
        for (const QJsonValue& e : entries) {
            std::vector<QString> row;
            const QJsonArray arr = e.toArray();
            row.reserve(arr.size());
            for (const QJsonValue& s : arr) row.push_back(s.toString());
            table.push_back(std::move(row));
        }

        // nameTable：可选；与 languages 位置对应（二维 [display][target]）。
        std::vector<std::vector<QString>> ntable;
        if (!nameTable.isEmpty()) {
            ntable.reserve(nameTable.size());
            for (const QJsonValue& e : nameTable) {
                std::vector<QString> row;
                const QJsonArray arr = e.toArray();
                row.reserve(arr.size());
                for (const QJsonValue& s : arr) row.push_back(s.toString());
                ntable.push_back(std::move(row));
            }
        }

        if (!table.empty()) {
            g_langNames    = std::move(names);
            g_langFamilies = std::move(fams);
            g_table        = std::move(table);
            g_nameTable    = std::move(ntable);
            // regions 是可选字段：{"欧洲": ["Europe", "欧洲", ...], ...}，
            // 数组下标与 languages 一一对应；缺失时 langRegionName 回退原始名。
            g_regionTable.clear();
            const QJsonObject regions = obj.value(QLatin1String("regions")).toObject();
            for (auto it = regions.begin(); it != regions.end(); ++it) {
                const QJsonArray arr = it.value().toArray();
                std::vector<QString> row;
                row.reserve(arr.size());
                for (const QJsonValue& v : arr) row.push_back(v.toString());
                g_regionTable.emplace_back(it.key(), std::move(row));
            }
            return;
        }
    }
}

int langCount() {
    return static_cast<int>(g_langNames.size());
}

QString langName(int i) {
    if (i >= 0 && i < static_cast<int>(g_langNames.size()))
        return g_langNames[i];
    return QString();
}

QString langName(int i, int uiLang) {
    if (i < 0 || i >= static_cast<int>(g_langNames.size()))
        return QString();
    // 越界或非法的 uiLang 直接回退到母语名称
    if (uiLang >= 0 && uiLang < static_cast<int>(g_nameTable.size())) {
        const std::vector<QString>& row = g_nameTable[uiLang];
        if (i < static_cast<int>(row.size()) && !row[i].isEmpty())
            return row[i];
    }
    return g_langNames[i];
}

QString langFamily(int i) {
    if (i >= 0 && i < static_cast<int>(g_langFamilies.size()))
        return g_langFamilies[i];
    return QString();
}

QStringList langFamilyGroups() {
    // 按语言表出现顺序去重得到一级大区（每个 family 取空格左侧）。
    QStringList seen;
    for (const QString& f : g_langFamilies) {
        if (f.isEmpty()) continue;
        const QString primary = f.section(QChar(' '), 0, 0);
        if (primary.isEmpty()) continue;
        if (!seen.contains(primary)) seen.append(primary);
    }
    return seen;
}

QString langRegionName(const QString& primary, int uiLang) {
    const auto find = [&]() -> const std::vector<QString>* {
        for (const auto& [name, row] : g_regionTable)
            if (name == primary) return &row;
        return nullptr;
    }();
    if (!find) return primary;
    const std::vector<QString>& row = *find;
    // 回退链：界面语言译文 → 英语译文 → 原始名
    if (uiLang >= 0 && uiLang < static_cast<int>(row.size())
        && !row[static_cast<size_t>(uiLang)].isEmpty())
        return row[static_cast<size_t>(uiLang)];
    if (!row.empty() && !row[0].isEmpty()) return row[0];
    return primary;
}

QStringView getlang(uint id, uint type) {
    if (type == 0xffffffffu) type = G.LANGUAGE;
    if (id < g_table.size()) {
        const std::vector<QString>& row = g_table[id];
        if (type < row.size())
            return QStringView(row[type]);   // 引用 g_table 中持久存活的 QString，安全
    }
    return NONETEXT;
}
