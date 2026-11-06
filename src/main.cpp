#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include "Common.h"
#include "app/KylinNoteApp.h"
#include "app/SingleInstanceGuard.h"
#include "app/TrayController.h"
#include "platform/InstanceChannel.h"
#include "ui/AppStyle.h"

int main(int argc, char* argv[]) {
    QCoreApplication::setApplicationName(QStringLiteral("kylinnote"));
    QCoreApplication::setApplicationVersion(QStringLiteral(KYLINNOTE_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("kylinnote"));
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("KylinNote 本地待办与桌面便签"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("background"), QStringLiteral("在后台启动（用于用户开启的登录自启）")});
    parser.addOption({QStringLiteral("data-dir"),
                      QStringLiteral("使用独立的数据目录（调试/便携运行）"),
                      QStringLiteral("directory")});
    parser.addOption(
        {QStringLiteral("smoke-test"), QStringLiteral("使用临时数据启动并在2秒后退出，供安装包自检")});
    parser.process(app);
    QTemporaryDir smokeData;
    if (parser.isSet(QStringLiteral("data-dir")))
        qputenv("KYLINNOTE_DATA_DIR", QDir(parser.value(QStringLiteral("data-dir"))).absolutePath().toUtf8());
    else if (parser.isSet(QStringLiteral("smoke-test")))
        qputenv("KYLINNOTE_DATA_DIR", smokeData.path().toUtf8());
    if (!qEnvironmentVariable("KYLINNOTE_DATA_DIR").isEmpty()) {
        // 独立调试目录同时隔离界面偏好，测试不修改真实用户配置。
        QSettings::setPath(
            QSettings::IniFormat, QSettings::UserScope, kn::dataDirPath() + QStringLiteral("/settings"));
    }
    kn::AppStyle::apply(app);
    app.setWindowIcon(kn::TrayController::trayIcon());
    kn::SingleInstanceGuard guard(kn::lockFilePath());
    const auto acquired = guard.acquire();
    if (acquired == kn::SingleInstanceGuard::AlreadyRunning) {
        QString error;
        if (!kn::InstanceChannel::notifyRunningInstance(&error))
            std::fprintf(stderr, "Cannot activate existing instance: %s\n", qPrintable(error));
        return 0;
    }
    if (acquired != kn::SingleInstanceGuard::Acquired) {
        std::fprintf(stderr, "Cannot create or lock data directory.\n");
        return 1;
    }
    kn::KylinNoteApp application;
    QString error;
    if (!application.init(&error, parser.isSet(QStringLiteral("background")))) {
        if (parser.isSet(QStringLiteral("smoke-test")))
            std::fprintf(stderr, "Startup failed: %s\n", qPrintable(error));
        else
            QMessageBox::critical(nullptr,
                                  QStringLiteral("无法启动 KylinNote"),
                                  error + QStringLiteral("\n请检查数据目录权限后重试。原有数据不会被覆盖。"));
        return 1;
    }
    if (parser.isSet(QStringLiteral("smoke-test")))
        QTimer::singleShot(2000, &app, &QCoreApplication::quit);
    const int result = app.exec();
    application.shutdown();
    guard.release();
    return result;
}
