/*
* Mainwindow include file
*/

#ifndef UNINSTALLER_MAINWINDOW_H
#define UNINSTALLER_MAINWINDOW_H
#include "registry.hpp"
#include "global.hpp"
#include "std.hpp"
#include "qt.hpp"

#include <vector>

using namespace std;

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
    void built_list();                          //获取软件信息
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
    void copyUninstallCommand();                //开发者：复制卸载命令
    void showDevInfo();                         //开发者：显示调试信息
    void setLanguage(int lang);                 //切换界面语言（0: en, 1: zh-cn）
    void retranslateUI();                       //重新套用所有静态文本（语言切换后调用）
    void locateInRegistry();                    //在注册表中定位当前条目（打开 regedit 并跳转）
    void setTheme(int t);                       //切换亮/暗主题（0: 亮, 1: 暗）
    void batchUninstall();                      //批量卸载选中项
    void batchDeleteResiduals();                //批量删除选中项的残留

private:
    void setupUI();
    void updateFindList();
    void loadLanguageSetting();                 //启动时从 QSettings 读取上次选择的语言
    void buildLanguageMenuItems();               //按 family 一级大区 + 二级系族构建语言菜单项
    SoftwareInfo* softwareAtRow(int row) const; // 通过表格行的 UserRole 取 SoftwareInfo*
    bool eventFilter(QObject* obj, QEvent* event) override; // 拦截表格右键
    void onTableContextMenu(const QPoint& pos); // 表格右键菜单
    void showDetailDialog(int row);             // 软件详情对话框（列出信息 + 功能按钮）
    void showUpdatePopup();                     // 启动时的更新日志弹窗（一打开主界面即弹出）
    bool isCriticalSystemItem(const SoftwareInfo* sw) const; // 系统关键项（更新/驱动/系统组件）拦截
    bool doUninstall(SoftwareInfo* software);      // 执行单个卸载（不含确认/预览），单条与批量共用

    // Members
    int len{ 0 };
    bool m_uiBuilt{ false };
    bool m_busy{ false };   // 防止卸载/扫描过程中重复触发
    bool m_showSystemComponents{ false }; // 默认隐藏系统组件，减少 VC++ 运行库等视觉噪音
    bool m_showOrphanOnly{ false };        // 仅显示残留项
    int m_theme{ 0 };                       // 0: 亮色, 1: 暗色
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
    vector<int> findlist{ 0, 1 };
    vector<SoftwareInfo> m_softwareList;
    map<int, vector<SoftwareInfo*>> m_swlist;
};

#endif //UNINSTALLER_MAINWINDOW_H
