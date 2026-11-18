#include "TypeSystem.h"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include "FontManager.h"

namespace kl {
namespace ds {
namespace {

// 随包注册的 OFL 字体优先于系统字体，确保目标机没有 Emoji 字体也能绘制。
QString bundledEmojiFamily() {
    static const QString family = [] {
        QStringList candidates;
        const QString override = qEnvironmentVariable("KYLINLIST_EMOJI_FONT");
        if (!override.isEmpty()) candidates.append(QFileInfo(override).absoluteFilePath());
        candidates.append(QCoreApplication::applicationDirPath()
                          + QStringLiteral("/fonts/NotoEmoji-Regular.ttf"));
#ifdef Q_OS_LINUX
        candidates.append(QStringLiteral("/usr/share/kylinlist/fonts/NotoEmoji-Regular.ttf"));
        candidates.append(QStringLiteral("/usr/local/share/kylinlist/fonts/NotoEmoji-Regular.ttf"));
#endif
        for (const QString& path : candidates) {
            const int id = QFontDatabase::addApplicationFont(path);
            if (id >= 0) return QFontDatabase::applicationFontFamilies(id).value(0);
        }
        return QString();
    }();
    return family;
}

}  // namespace

qreal pointSize(TypeScale scale) {
    switch (scale) {
        case TypeScale::Caption: return 9.0;
        case TypeScale::Body: return 10.5;
        case TypeScale::Menu: return 9.75;
        case TypeScale::MenuCaption: return 9.0;
        case TypeScale::Subtitle: return 10.5;
        case TypeScale::Title: return 20.0;
        case TypeScale::Display: return 24.0;
        case TypeScale::DetailTitle: return 14.0;
        case TypeScale::Count: break;
    }
    return pointSize(TypeScale::Body);
}

QStringList fontFamilies() {
    // 中文界面直接选择可缩放的中文无衬线族，避免 Segoe UI 的系统回退落到点阵宋体。
#ifdef Q_OS_WIN
    return {QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Microsoft YaHei"),
            QStringLiteral("Noto Sans CJK SC"), QStringLiteral("Source Han Sans SC"),
            QStringLiteral("Source Han Sans CN"), QStringLiteral("Segoe UI"),
            QStringLiteral("sans-serif")};
#else
    return {QStringLiteral("Noto Sans CJK SC"), QStringLiteral("Source Han Sans SC"),
            QStringLiteral("Source Han Sans CN"), QStringLiteral("WenQuanYi Micro Hei"),
            QStringLiteral("PingFang SC"), QStringLiteral("Hiragino Sans GB"),
            QStringLiteral("sans-serif")};
#endif
}

namespace {
QString resolveFamily() {
    return FontManager::instance().family();
}
}  // namespace

QFont qfont(FontScene scene) {
    QFont f(resolveFamily());
    f.setStyleHint(QFont::SansSerif);
    switch (scene) {
        case FontScene::Base:
            f.setPointSizeF(pointSize(TypeScale::Body));
            break;
        case FontScene::Body:
            f.setPointSizeF(pointSize(TypeScale::Body));
            break;
        case FontScene::Display:
            f.setPointSizeF(pointSize(TypeScale::Title));
            f.setWeight(QFont::DemiBold);
            break;
        case FontScene::Section:
            f.setPointSizeF(pointSize(TypeScale::Body));
            f.setWeight(QFont::DemiBold);
            break;
        case FontScene::Caption:
            f.setPointSizeF(pointSize(TypeScale::Caption));
            break;
        case FontScene::Mono:
            f.setFamily(QStringLiteral("Consolas"));
            f.setPointSizeF(pointSize(TypeScale::Body));
            f.setStyleHint(QFont::Monospace);
            break;
        case FontScene::DetailTitle:
            f.setPointSizeF(pointSize(TypeScale::DetailTitle));
            f.setWeight(QFont::Bold);
            break;
        case FontScene::ListSection:
            f.setPointSizeF(pointSize(TypeScale::Subtitle));
            f.setWeight(QFont::DemiBold);
            break;
        case FontScene::Menu:
            f.setPointSizeF(pointSize(TypeScale::Menu));
            break;
        case FontScene::MenuCaption:
            f.setPointSizeF(pointSize(TypeScale::MenuCaption));
            break;
        case FontScene::Count:
            break;
    }
    f.setStyleStrategy(QFont::StyleStrategy(QFont::PreferAntialias | QFont::PreferOutline));
    return f;
}

QString bundledEmojiFamilyName() { return bundledEmojiFamily(); }

QFont emojiFont() {
    QFont font = qfont(FontScene::Body);
    const QString bundled = bundledEmojiFamily();
    if (!bundled.isEmpty()) {
        font.setFamily(bundled);
        font.setStyleStrategy(QFont::NoFontMerging);
        return font;
    }
    const QStringList available = QFontDatabase().families();
    for (const QString& family : {QStringLiteral("Segoe UI Emoji"),
                                  QStringLiteral("Noto Color Emoji"),
                                  QStringLiteral("Apple Color Emoji"),
                                  QStringLiteral("Twemoji Mozilla")}) {
        if (available.contains(family)) {
            font.setFamily(family);
            break;
        }
    }
    return font;
}

}  // namespace ds
}  // namespace kl
