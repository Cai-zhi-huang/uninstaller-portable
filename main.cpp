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

int main(int argc, char *argv[])
{
	QApplication app(argc, argv);

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
		const QString logPath = QCoreApplication::applicationDirPath() + QStringLiteral("/startup.log");
		QFile f(logPath);
		if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
			QTextStream ts(&f);
			ts << QDateTime::currentDateTime().toString(Qt::ISODate)
			   << "  langCount=" << langCount()
			   << "  families=" << langFamilyGroups().size()
			   << '\n';
		}
	}

	UninstallerWindow window;
	window.run();

	return app.exec();
}
