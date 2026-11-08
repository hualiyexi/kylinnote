#include "TypeSystem.h"
#include <QApplication>
#include <QFontDatabase>

namespace kl {
namespace ds {

qreal pointSize(TypeScale scale) {
    switch (scale) {
        case TypeScale::Caption: return 8.5;
        case TypeScale::Body: return 9.5;
        case TypeScale::Subtitle: return 10.5;
        case TypeScale::Title: return 20.0;
        case TypeScale::Display: return 24.0;
        case TypeScale::Count: break;
    }
    return 9.5;
}

QStringList fontFamilies() {
    // 复用现有探测链，升格为回退列表。
    return {QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Microsoft YaHei"),
            QStringLiteral("Noto Sans CJK SC"), QStringLiteral("WenQuanYi Micro Hei")};
}

namespace {
QString resolveFamily() {
    static const QString resolved = [] {
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
            break;
        case FontScene::Count:
            break;
    }
    return f;
}

}  // namespace ds
}  // namespace kl
