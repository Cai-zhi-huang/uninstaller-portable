/*
* The project *Unsinstaller* which aim to uninstall the software cleanly.
*/

#include "mainwindow.hpp"
#include "version.hpp"
#include "language.hpp"
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QCoreApplication>

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char *argv[])
{
#ifdef _WIN32
    // 安全加固（F6）：从 DLL 搜索顺序中移除“当前工作目录”，并启用安全搜索模式，
    // 防止 exe 同级/可写目录被植入同名恶意 DLL（如 Qt6Core.dll）时被加载执行（DLL 种植劫持）。
    // 配合 app.manifest 的 asInvoker 与按需 runas，把“放一个 DLL 就接管进程”的攻击面降到最低。
    ::SetDllDirectoryW(L"");
    ::SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE);
#endif

	QApplication app(argc, argv);
	// 关闭最后一个窗口即退出整个程序（含后台），不残留进程
	app.setQuitOnLastWindowClosed(true);

	// 加载 exe 内嵌的程序图标（IDI_APP_ICON），作为窗口标题栏与任务栏图标
	app.setWindowIcon(QIcon(":/appicon.ico"));
	// 设置应用名称与版本，便于系统/任务管理器识别（“像正常软件一样”）。
	app.setApplicationName(QString::fromUtf8(u8"卸载管理器"));
	app.setApplicationDisplayName(QString::fromUtf8(u8"卸载管理器"));
	app.setApplicationVersion(QString::fromUtf8(APP_VERSION));

	// 运行时加载语言表（exe 同级 lang/languages.json 或内嵌资源），必须在建界面之前完成
	loadLanguageTable();

	// 自检：把实际加载到的语言数 + 数据来源写到 exe 同级 startup.log。
	// 用来回答"我装的是不是最新版"——只要数对得上 108，那就是新版本。
	{
		const QString line = QDateTime::currentDateTime().toString(Qt::ISODate)
			+ QStringLiteral("  langCount=") + QString::number(langCount())
			+ QStringLiteral("  families=") + QString::number(langFamilyGroups().size());
		appendStartupLog(line);
	}

	UninstallerWindow window;
	window.run();

	return app.exec();
}
