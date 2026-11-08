#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include "Common.h"
#include "app/KylinListApp.h"
#include "app/SingleInstanceGuard.h"
#include "app/TrayController.h"
#include "platform/InstanceChannel.h"
#include "ui/AppStyle.h"

int main(int argc, char* argv[]) {
    QCoreApplication::setApplicationName(QStringLiteral("kylinlist"));
    QCoreApplication::setApplicationVersion(QStringLiteral(KYLINLIST_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("kylinlist"));
    QApplication::setApplicationDisplayName(QStringLiteral("麒麟清单"));
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("麒麟清单：本地任务、日程与桌面便签"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("background"), QStringLiteral("在后台启动（用于用户开启的登录自启）")});
    parser.addOption({QStringLiteral("data-dir"),
                      QStringLiteral("使用独立的数据目录（调试/便携运行）"),
                      QStringLiteral("directory")});
    parser.addOption(
        {QStringLiteral("smoke-test"), QStringLiteral("使用临时数据启动并在2秒后退出，供安装包自检")});
    parser.process(app);
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
    if (!application.init(&error, parser.isSet(QStringLiteral("background")))) {
        if (parser.isSet(QStringLiteral("smoke-test")))
            std::fprintf(stderr, "Startup failed: %s\n", qPrintable(error));
        else
            QMessageBox::critical(nullptr,
                                  QStringLiteral("无法启动麒麟清单"),
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
