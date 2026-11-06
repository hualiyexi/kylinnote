#include "Common.h"
#include "app/DbusAdapter.h"
#include "app/KylinNoteApp.h"
#include "app/SingleInstanceGuard.h"

#include <QApplication>
#include <QFont>
#include <QGuiApplication>
#include <QMessageBox>
#include <cstdio>

int main(int argc, char *argv[])
{
    QCoreApplication::setApplicationName(QStringLiteral("kylinnote"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));
    QCoreApplication::setOrganizationName(QStringLiteral("kylinnote"));
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, true);
    // 关闭最后一个窗口不退出：全部便签隐藏后仍需保留托盘入口
    QApplication::setQuitOnLastWindowClosed(false);

    QApplication app(argc, argv);

    QFont font(QStringLiteral("WenQuanYi Micro Hei"), kn::DefaultFontPt);
    font.setStyleHint(QFont::SansSerif);
    QApplication::setFont(font);

    if (QGuiApplication::platformName() != QLatin1String("x11")) {
        std::fprintf(stderr, "[kylinnote] 当前会话平台为 %s，本程序针对 X11 设计\n",
                     qPrintable(QGuiApplication::platformName()));
    }

    // 单实例：先抢文件锁，抢不到就通过 D-Bus 让已运行的实例"显示全部"
    kn::SingleInstanceGuard guard(kn::lockFilePath());
    kn::SingleInstanceGuard::Result guardResult = guard.acquire();
    if (guardResult != kn::SingleInstanceGuard::Acquired) {
        QString dbusError;
        if (kn::DbusAdapter::notifyRunningInstance(&dbusError)) {
            return 0;
        }
        std::fprintf(stderr, "[kylinnote] 已有实例在运行，但无法通过 D-Bus 唤醒: %s\n",
                     qPrintable(dbusError));
        return 0;
    }

    kn::KylinNoteApp kylinNote;
    QString initError;
    if (!kylinNote.init(&initError)) {
        QMessageBox::critical(nullptr, QStringLiteral("KylinNote"), initError);
        return 1;
    }

    const int rc = app.exec();
    kylinNote.shutdown();
    guard.release();
    return rc;
}
