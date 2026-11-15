#include "TypeSystem.h"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>

namespace kl {
namespace ds {
namespace {

// 可选的内置字体目录。若发布包在 <应用目录>/fonts 放入 OFL/开源授权的
// TTF/OTF，启动时自动注册；未放置时完全走系统字体链，不产生额外依赖。
// 这样既支持「目标机缺中文字体」的离线兜底，也不强制增大安装包体积。
int loadBundledFonts() {
    const QString dir = QCoreApplication::applicationDirPath() + QStringLiteral("/fonts");
    if (!QDir(dir).exists()) return 0;
    int loaded = 0;
    for (const QFileInfo& info : QDir(dir).entryInfoList(
             {QStringLiteral("*.ttf"), QStringLiteral("*.otf")}, QDir::Files)) {
        if (QFontDatabase::addApplicationFont(info.absoluteFilePath()) >= 0) ++loaded;
    }
    return loaded;
}

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
        case TypeScale::Caption: return 8.5;
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
    // 与滴答一致使用系统无衬线主字体，再回退中文与通用字体。
    // 内置字体（若发布包提供）注册后会以这些族名之一出现在 QFontDatabase 中。
    return {QStringLiteral("Segoe UI"), QStringLiteral("Noto Sans CJK SC"),
            QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Microsoft YaHei"),
            QStringLiteral("Source Han Sans SC"), QStringLiteral("Source Han Sans CN"),
            QStringLiteral("WenQuanYi Micro Hei"), QStringLiteral("PingFang SC"),
            QStringLiteral("Hiragino Sans GB"), QStringLiteral("sans-serif")};
}

namespace {
QString resolveFamily() {
    static const QString resolved = [] {
        loadBundledFonts();  // 先注册可选内置字体，再探测
        const QStringList available = QFontDatabase().families();
        for (const QString& name : fontFamilies())
            if (available.contains(name)) return name;
        return QApplication::font().family();
    }();
    return resolved;
}
}  // namespace

QFont qfont(FontScene scene) {
    QFont f(resolveFamily());
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
