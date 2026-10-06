/*
* Mainwindow include file
*/

#ifndef UNINSTALLER_MAINWINDOW_H
#define UNINSTALLER_MAINWINDOW_H
#include "registry.hpp"
#include "global.hpp"
#include "std.hpp"
#include "qt.hpp"
#include <QCloseEvent>
#include <QSystemTrayIcon>

class QNetworkAccessManager;
class QNetworkReply;
class QVBoxLayout;

#include <vector>

using namespace std;

// 导出用的单行数据结构（导出全部与导出选中共用）
struct ExportRow { QString name, ver, pub, date, size, loc, status, cmd; };

// 诊断日志追加（带体积上限，定义在 mainwindow.cpp）；main.cpp 自检日志复用
void appendStartupLog(const QString& line);

// 结构化安全事件日志（定义在 mainwindow.cpp）：每行一条 JSON，供威胁检测系统采集。
// 与 threat_detection_plan.md 的 R3(缓存校验失败)/R5(越界删除被拒)/R6(删键被拒) 规则对应。
void logSecurityEvent(const QString& rule, const QString& eventType, const QString& detail);

// 名称列搜索高亮委托（定义在 mainwindow.cpp，全局类，前向声明供成员指针使用）
class HighlightDelegate;

class UninstallerWindow : public QMainWindow // Should be QMainWindow
{
    Q_OBJECT

public:
    void run();
    void fresh();
    explicit UninstallerWindow(QWidget* parent = nullptr);

private slots:
    bool tick(const ll& row);                   //选择检查
    void sorting();                             //排序
    void built_list(bool allowCache = false);   //获取软件信息（allowCache: 启动时命中1小时内缓存则跳过扫描）
    void showDetails();                         //显示详细信息
    void openFileLocation();                    //打开文件所在位置（详情/右键菜单）
    void scanResiduals();                       //扫描残留
    void filterSoftware();
    void loadSoftwareList(); //加载
    void uninstallSelected();
    void startBackgroundIconLoad();            // 后台线程懒加载真实图标（解耦进度条）
    void onIconReady(qlonglong swPtr, const QImage& img); // 后台图标加载完成后的回写
    void toggleShowSystemComponents();          //开发者：切换显示系统组件
    void deleteRegistryEntry();                 //删除残留注册表项（清理僵尸条目）
    void forceDeleteEntry();                    //强制删除此条目（绕过残留检测，删除注册表项与磁盘残留）
    void uninstallSelf();                       //卸载本程序（自卸载，由独立 uninst.exe 负责删目录）
    void showAbout();                           //关于本程序（展示版本号）
    void exportSoftwareList();                  //开发者：导出软件列表
    void exportSelectedSoftware();              //右键菜单：仅导出当前选中行
    void copyUninstallCommand();                //开发者：复制卸载命令
    void showDevInfo();                         //开发者：显示调试信息
    void setLanguage(int lang);                 //切换界面语言（0: en, 1: zh-cn）
    void retranslateUI();                       //重新套用所有静态文本（语言切换后调用）
    void locateInRegistry();                    //在注册表中定位当前条目（打开 regedit 并跳转）
    void setTheme(int t);                       //切换亮/暗主题（0: 亮, 1: 暗）
    void setCustomBackground();                 //视图：选择自定义背景图片
    void clearCustomBackground();               //视图：清除自定义背景，恢复默认
    void setBgDim(int level);                   //视图：背景浓度（0 淡 / 1 中 / 2 浓）
    void setBgMode(int mode);                   //视图：背景模式（0 铺满 / 1 居中 / 2 平铺）
    void setBgBlur(bool on);                    //视图：背景模糊开关
    void useDesktopWallpaper();                 //视图：使用系统桌面壁纸作为背景
    void batchUninstall();                      //批量卸载选中项
    void batchDeleteResiduals();                //批量删除选中项的残留
    void checkForUpdate();                      //启动后异步拉取服务器版本信息（不阻塞界面）
    void onVersionReplyFinished(QNetworkReply* reply); // 版本检查网络回调
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason); // 托盘图标点击
    void showHistoryDialog();                   // 查看操作历史（导出 txt/csv）
    void showSizeRanking();                     // 磁盘占用排行（Top15 条形图）
    void scanAllResiduals();                    // 扫描所有残留项并批量清理
    void setAutoStart(bool on);                 // 开机自启动开关（HKCU Run）

private:
    void setupUI();
    bool loadSoftwareCache();                   // 读取1小时内软件列表缓存，命中返回 true
    void saveSoftwareCache();                   // 将当前软件列表写入缓存（含时间戳）
    void updateFindList();
    void loadLanguageSetting();                 //启动时从 QSettings 读取上次选择的语言
    void buildLanguageMenuItems();               //按 family 一级大区 + 二级系族构建语言菜单项
    SoftwareInfo* softwareAtRow(int row) const; // 通过表格行的 UserRole 取 SoftwareInfo*
    QVector<ExportRow> collectExportRows(bool selectedOnly) const; // 收集导出行（selectedOnly=true 仅选中行）
    bool eventFilter(QObject* obj, QEvent* event) override; // 拦截表格右键
    void onTableContextMenu(const QPoint& pos); // 表格右键菜单
    void showDetailDialog(int row);             // 软件详情对话框（列出信息 + 功能按钮）
    void showUpdatePopup();                     // 启动时的更新日志弹窗（一打开主界面即弹出）
    QString loadChangelogLatest();               // 从 CHANGELOG.md 解析最新版本段（回退内置文案）
    // —— 自定义背景 ——
    void loadCustomBgSettings();                // 启动时读取背景设置并加载图片
    void loadDesktopWallpaper();                // 读取系统桌面壁纸路径并加载为背景
    QPixmap blurPixmap(const QPixmap& src) const; // 简单盒式模糊（背景柔和化）
    QString bgOverlaySheet() const;             // 有背景图时追加到主题 QSS 的半透明化规则
    void syncBgMenuChecks();                    // 同步背景相关菜单的勾选/可用状态
    QNetworkAccessManager* m_netMgr{ nullptr };  // 版本检查用的网络管理器（随窗口生命周期）
    QDialog* m_updateDlg{ nullptr };             // 当前打开的更新弹窗（网络回调据此追加新版本提示）
    QVBoxLayout* m_updateLayout{ nullptr };      // 更新弹窗的主布局（用于追加横幅）
    void closeEvent(QCloseEvent* event) override; // 关闭前台即关闭整个程序（含后台），不缩托盘
    void paintEvent(QPaintEvent* event) override;  // 自定义背景：先画图+遮罩，子控件半透明透出
    void resizeEvent(QResizeEvent* event) override; // 窗口尺寸变化时重算背景缓存
    bool isCriticalSystemItem(const SoftwareInfo* sw) const; // 系统关键项（更新/驱动/系统组件）拦截
    UninstallResult doUninstall(SoftwareInfo* software, bool showProgress = true); // 执行单个卸载（不含确认/预览），单条与批量共用
    void showResidualCleanup(SoftwareInfo* software, const std::vector<std::string>& residuals, bool force); // 残留清理对话框（单条）
    void autoScanResidualsAfterUninstall(SoftwareInfo* software); // 卸载成功后自动扫描残留（force=true）
    void showBatchResidualCleanup(const QList<SoftwareInfo*>& uninstalled); // 批量卸载后的合并残留清理对话框
    void createTray();                          // 初始化系统托盘（关闭最小化到托盘）
    void jumpToSoftware(SoftwareInfo* sw);       // 在列表中定位并选中某软件（占用排行点击跳转）
    bool isAutoStart() const;                    // 读取 HKCU Run 判断是否开机自启
    QString opLogPath() const;                  // 操作历史日志文件路径（AppData/Local）
    void logOperation(const QString& op, const QString& name, bool success, const QString& detail); // 追加一条操作记录

    // Members
    int len{ 0 };
    bool m_uiBuilt{ false };
    bool m_busy{ false };   // 防止卸载/扫描过程中重复触发
    bool m_showSystemComponents{ false }; // 默认隐藏系统组件，减少 VC++ 运行库等视觉噪音
    bool m_showOrphanOnly{ false };        // 仅显示残留项
    bool m_columnsSized{ false };           // 列宽是否已自动调整过（仅首次 loadSoftwareList 调整，避免重扫重置用户列宽）
    int m_theme{ 0 };                       // 0: 亮色, 1: 暗色
    bool m_forceQuit{ false };              // 托盘“退出”触发，绕过最小化到托盘逻辑
    int m_statusFilter{ 0 };                // 状态筛选：0 全部 / 1 正常 / 2 运行中 / 3 残留
    QSystemTrayIcon* m_tray{ nullptr };     // 系统托盘图标
    QMenu* m_trayMenu{ nullptr };           // 托盘右键菜单
    QComboBox* m_statusCombo{ nullptr };    // 工具栏状态筛选下拉
    HighlightDelegate* m_hlDelegate{ nullptr }; // 名称列搜索高亮委托
    QStringList m_hlTokens;                  // 当前搜索高亮词（原始输入分词）
    QAction* m_colActs[7]{ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr }; // 列显隐开关
    QAction* m_autostartAct{ nullptr };      // “开机自启动”勾选项
    QMenu* actionMenu{ nullptr };
    QMenu* m_selfMenu{ nullptr };          //“本程序”菜单
    QMenu* m_devMenu{ nullptr };           //“开发者”菜单
    QMenu* m_langMenu{ nullptr };          //“语言”菜单
    QMenu* m_viewMenu{ nullptr };          //“视图/主题”菜单
    QAction* m_showSystemAction{ nullptr };
    QAction* m_selfUninstallAction{ nullptr };
    QAction* m_aboutAction{ nullptr };
    QAction* m_locateAction{ nullptr };    // 右键“在注册表中定位”
    QAction* m_themeAction{ nullptr };     // 主题切换动作
    QAction* m_lightThemeAct{ nullptr };   // “亮色主题”菜单项（互斥单选）
    QAction* m_darkThemeAct{ nullptr };    // “暗色主题”菜单项（互斥单选）
    QAction* m_bgSetAct{ nullptr };        // “自定义背景图片…”菜单项
    QAction* m_bgClearAct{ nullptr };      // “恢复默认背景”菜单项
    QAction* m_bgDimActs[3]{ nullptr, nullptr, nullptr };
    QAction* m_bgModeActs[3]{ nullptr, nullptr, nullptr };
    QAction* m_bgBlurAct{ nullptr };
    QAction* m_bgDesktopAct{ nullptr }; // 背景浓度三档（互斥单选）
    QPixmap m_bgPixmap;                    // 自定义背景原图（cover 缩放在 paintEvent 中做）
    QPixmap m_bgScaled;                    // 按窗口尺寸缓存的渲染结果（cover 含模糊）
    QPixmap m_bgCenterCache;               // 居中模式的模糊缓存（原图尺寸，不随窗口变）
    QSize m_bgScaledFor;                   // m_bgScaled 对应的窗口尺寸
    int m_bgDim{ 1 };
    int m_bgMode{ 0 };
    bool m_bgBlur{ false };
    bool m_bgFollowDesktop{ false };                      // 背景遮罩浓度：0 淡 / 1 中 / 2 浓
    QComboBox* m_langCombo{ nullptr };   // 工具栏右侧语言下拉框
    // 语言菜单中每个语言项的 QAction，与 langCount() 一一对应（按 langIndex 索引），
    // 用于 setLanguage 直接定位无需遍历嵌套 submenu。失败/未分配的槽位保留 nullptr。
    std::vector<QAction*> m_langActions;
    QMenuBar* bar{ nullptr };
    QLabel* m_scanLabel{ nullptr };        //工具栏“搜索:”标签
    QCheckBox* m_orphanOnlyCheck{ nullptr };
    QPushButton* m_refreshBtn{ nullptr };
    QPushButton* m_uninstallBtn{ nullptr };
    QPushButton* m_scanBtn{ nullptr };
    QPushButton* m_detailsBtn{ nullptr };
    QPushButton* m_batchUninstallBtn{ nullptr }; // 批量卸载
    QPushButton* m_batchDelBtn{ nullptr };       // 批量删除残留
    filesize_t total_size;
    QLineEdit* m_searchEdit{ nullptr };
    QTableWidget* m_tableWidget{ nullptr };
    QThread* m_iconThread{ nullptr };       // 后台图标加载线程（关闭窗口时需强制退出，防进程残留）
    vector<int> findlist{ 0, 1 };
    vector<SoftwareInfo> m_softwareList;
    map<int, vector<SoftwareInfo*>> m_swlist;
};

#endif //UNINSTALLER_MAINWINDOW_H
