#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QMessageBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include "Common.h"
#include "app/DemoData.h"
#include "app/KylinListApp.h"
#include "app/Branding.h"
#include "app/SingleInstanceGuard.h"
#include "app/TrayController.h"
#include "platform/InstanceChannel.h"
#include "platform/SystemShadow.h"
#ifdef Q_OS_LINUX
#include "platform/DesktopBranding.h"
#endif
#include "ui/AppStyle.h"

namespace {
// 演示数据工具只认显式选项；--data-dir 必须是绝对路径且指向全新空目录（seed）或已生成的
// 演示目录（verify）。整条路径只用 QCoreApplication，绝不初始化 QApplication/Branding/
// KylinListApp，绝不触碰默认用户数据目录，锁被占用时直接失败而不激活其他实例。
bool hasCliOption(int argc, char* argv[], const char* key)
{
    const QString name = QString::fromLocal8Bit(key);
    for (int i = 1; i < argc; ++i)
        if (QString::fromLocal8Bit(argv[i]) == name) return true;
    return false;
}

int runDemoTool(int argc, char* argv[])
{
    QCoreApplication coreApp(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("KylinList 合成演示数据工具"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("seed-demo"), QStringLiteral("在全新的空目录中生成合成演示数据")});
    parser.addOption({QStringLiteral("verify-demo"), QStringLiteral("校验已生成的演示数据目录")});
    parser.addOption({QStringLiteral("data-dir"), QStringLiteral("数据目录（必须是绝对路径）"),
                      QStringLiteral("directory")});
    if (!parser.parse(QCoreApplication::arguments())) {
        std::fprintf(stderr, "用法错误：%s\n用 %s --seed-demo --data-dir <绝对路径> 或 "
                             "--verify-demo --data-dir <绝对路径>\n",
                     qPrintable(parser.errorText()), argv[0]);
        return 2;
    }
    if (parser.isSet(QStringLiteral("version"))) parser.showVersion();
    if (parser.isSet(QStringLiteral("help"))) parser.showHelp();
    const bool seed = parser.isSet(QStringLiteral("seed-demo"));
    const bool verify = parser.isSet(QStringLiteral("verify-demo"));
    const QString dataDir = parser.value(QStringLiteral("data-dir"));
    if (seed == verify || dataDir.trimmed().isEmpty() || !QDir::isAbsolutePath(dataDir.trimmed())) {
        std::fprintf(stderr,
                     "用法错误：--seed-demo 与 --verify-demo 只能二选一，且必须显式提供绝对 --data-dir <dir>\n");
        return 2;
    }
    QString error;
    if (seed) {
        bool locked = false;
        if (!kl::demo::generate(dataDir, &error, &locked)) {
            std::fprintf(stderr, "seed-demo 失败：%s\n", qPrintable(error));
            return locked ? 3 : 1;
        }
        std::fprintf(stdout, "seed-demo 完成：%s（demo-manifest.json 已写入）\n",
                     qPrintable(QDir(dataDir.trimmed()).absolutePath()));
        return 0;
    }
    QString report;
    bool verifyLocked = false;
    if (!kl::demo::verify(dataDir, &report, &error, &verifyLocked)) {
        std::fprintf(stderr, "verify-demo 失败：%s\n", qPrintable(error));
        if (!report.isEmpty()) std::fprintf(stdout, "%s\n", qPrintable(report));
        const QString written = kl::demo::reportPath(dataDir.trimmed());
        if (QFile::exists(written))
            std::fprintf(stderr, "完整报告：%s\n", qPrintable(written));
        return verifyLocked ? 3 : 1;
    }
    std::fprintf(stdout, "%s\n", qPrintable(report));
    return 0;
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication::setApplicationName(QStringLiteral("kylinlist"));
    QCoreApplication::setApplicationVersion(QStringLiteral(KYLINLIST_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("kylinlist"));
    if (hasCliOption(argc, argv, "--seed-demo") || hasCliOption(argc, argv, "--verify-demo"))
        return runDemoTool(argc, argv);
#ifdef Q_OS_LINUX
    // deb 安装/卸载钩子没有 DISPLAY，只需 QCoreApplication 来写系统菜单覆盖。
    const bool refreshSystemBranding = argc == 2
        && qstrcmp(argv[1], "--refresh-system-branding") == 0;
    const bool clearSystemBranding = argc == 2
        && qstrcmp(argv[1], "--clear-system-branding") == 0;
    if (refreshSystemBranding || clearSystemBranding) {
        QCoreApplication coreApp(argc, argv);
        QString error;
        const bool ok = refreshSystemBranding
            ? kl::DesktopBranding::refreshSystem(&error)
            : kl::DesktopBranding::clearSystem(&error);
        if (!ok)
            std::fprintf(stderr, "Desktop branding: %s\n", qPrintable(error));
        return ok ? 0 : 1;
    }
#endif
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    kl::Branding::load();
    QApplication::setApplicationDisplayName(kl::Branding::displayName());
    app.setQuitOnLastWindowClosed(false);
    // 所有窗口阴影由应用自绘：禁止系统给无边框浮层补直角投影。
    kl::disableNativeWindowShadows();
    QCommandLineParser parser;
    parser.setApplicationDescription(kl::Branding::displayName() + QStringLiteral("：本地任务、日程与桌面便签"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("background"), QStringLiteral("在后台启动，恢复桌面便签与任务小部件")});
    parser.addOption({QStringLiteral("data-dir"),
                      QStringLiteral("使用独立的数据目录（调试/便携运行）"),
                      QStringLiteral("directory")});
    parser.addOption({QStringLiteral("preview-autostart"),
                      QStringLiteral("允许在设置中为该数据目录手动登记预览开机自启"
                                     "（必须与显式 --data-dir 同时使用，写入独立的"
                                     "预览自启项，不影响正式安装的自启）")});
    parser.addOption(
        {QStringLiteral("smoke-test"), QStringLiteral("使用临时数据启动并在2秒后退出，供安装包自检")});
    parser.process(app);
    // 预览自启必须显式开启：只有用户主动加上 --preview-autostart 且同时显式
    // 指定 --data-dir 的隔离运行才允许登记预览自启项；smoke-test 绝不登记，
    // 普通的 --data-dir 调试运行也不登记（开关在设置中呈禁用态）。
    const bool previewAutostart = parser.isSet(QStringLiteral("preview-autostart"));
    if (previewAutostart
        && (!QDir::isAbsolutePath(parser.value(QStringLiteral("data-dir")))
            || parser.isSet(QStringLiteral("smoke-test")))) {
        std::fprintf(stderr,
                     "用法错误：--preview-autostart 必须与显式 --data-dir <绝对路径> "
                     "同时使用，且不能与 --smoke-test 同时使用。\n");
        return 2;
    }
    if (parser.isSet(QStringLiteral("data-dir")))
        qputenv("KYLINLIST_DATA_DIR", QDir(parser.value(QStringLiteral("data-dir"))).absolutePath().toUtf8());
    else if (parser.isSet(QStringLiteral("smoke-test"))) {
        // 每次独立目录，避免重复自检被上一个实例的锁或数据库影响。
        const QString smokeDir = QDir::current().absoluteFilePath(QStringLiteral("smoke-data"));
        if (!QDir().mkpath(smokeDir)) {
            std::fprintf(stderr, "Cannot create smoke-test data directory.\n");
            return 1;
        }
        QTemporaryDir smokeData(smokeDir + QStringLiteral("/run-XXXXXX"));
        if (smokeData.isValid())
            qputenv("KYLINLIST_DATA_DIR", smokeData.path().toUtf8());
        smokeData.setAutoRemove(false);
        if (smokeData.path().isEmpty() || !QDir(smokeData.path()).exists()) {
            std::fprintf(stderr, "Cannot create smoke-test data directory.\n");
            return 1;
        }
    }
    if (!qEnvironmentVariable("KYLINLIST_DATA_DIR").isEmpty()) {
        // 独立调试目录同时隔离界面偏好，测试不修改真实用户配置。
        QSettings::setPath(
            QSettings::IniFormat, QSettings::UserScope, kl::dataDirPath() + QStringLiteral("/settings"));
    }
    // --smoke-test / --data-dir / KYLINLIST_DATA_DIR 都是隔离运行：
    // 不写真实开机自启，也不自动上报测试统计。
    const bool isolatedRun = parser.isSet(QStringLiteral("smoke-test"))
        || !qEnvironmentVariable("KYLINLIST_DATA_DIR").isEmpty();
    kl::AppStyle::apply(app);
    app.setWindowIcon(kl::TrayController::trayIcon());
    kl::SingleInstanceGuard guard(kl::lockFilePath());
    const auto acquired = guard.acquire();
    if (acquired == kl::SingleInstanceGuard::AlreadyRunning) {
        QString error;
        if (!kl::InstanceChannel::notifyRunningInstance(&error))
            std::fprintf(stderr, "Cannot activate existing instance: %s\n", qPrintable(error));
        return 0;
    }
    if (acquired != kl::SingleInstanceGuard::Acquired) {
        std::fprintf(stderr, "Cannot create or lock data directory.\n");
        return 1;
    }
    kl::KylinListApp application;
    QString error;
    if (!application.init(&error, parser.isSet(QStringLiteral("background")), isolatedRun,
                          previewAutostart)) {
        if (parser.isSet(QStringLiteral("smoke-test")))
            std::fprintf(stderr, "Startup failed: %s\n", qPrintable(error));
        else
            QMessageBox::critical(nullptr,
                                  QStringLiteral("无法启动%1").arg(kl::Branding::displayName()),
                                  error + QStringLiteral("\n请检查数据目录权限后重试。原有数据不会被覆盖。"));
        return 1;
    }
    if (parser.isSet(QStringLiteral("smoke-test"))) {
        QTimer::singleShot(2000, &app, [&app]() {
            std::fprintf(stdout, "KylinList smoke-test: initialization and SQLite succeeded.\n");
            std::fflush(stdout);
            app.exit(0);
        });
    }
    const int result = app.exec();
    application.shutdown();
    guard.release();
    return result;
}
