#include "ColorTheme.h"

#include <QWidget>

namespace kn {

ColorTheme::Spec ColorTheme::spec(NoteColor c) {
    switch (c) {
        case NoteColor::Yellow:
            return {"yellow", "浅黄", "#FFF8D9", "#F7EDBD", "#D8C886", "#3C382C", "#5B5337", "#E3D79D"};
        case NoteColor::Blue:
            return {"blue", "蓝色", "#C6EAF8", "#A9DCEF", "#8EC9DE", "#1F2B33", "#27414F", "#B2DFF2"};
        case NoteColor::Green:
            return {"green", "绿色", "#D2F0C8", "#B7E3A9", "#99D08A", "#22301C", "#35502C", "#C1E8B4"};
        case NoteColor::Pink:
            return {"pink", "粉色", "#FBDCEA", "#F5C2DA", "#E9A8C4", "#331E28", "#542F3E", "#F6CDE0"};
        case NoteColor::Purple:
            return {"purple", "紫色", "#E0D2F2", "#CBB9E7", "#B29FD6", "#251C34", "#3E2E56", "#D5C4ED"};
        case NoteColor::White:
        default:
            return {"white", "白色", "#FFFFFF", "#EFEFEF", "#D6D6D6", "#2B2B2B", "#4A4A4A", "#DCE7F5"};
    }
}

NoteColor ColorTheme::next(NoteColor c) {
    const int v = (toInt(c) + 1) % ColorCount;
    return fromInt(v);
}

NoteColor ColorTheme::fromInt(int v) {
    if (v < 0 || v >= ColorCount) {
        return NoteColor::Yellow;
    }
    return static_cast<NoteColor>(v);
}

int ColorTheme::toInt(NoteColor c) {
    return static_cast<int>(c);
}

QString ColorTheme::windowStyle(NoteColor c) {
    const Spec s = spec(c);
    // 类位于 kn 命名空间，QSS 类型选择器需写成 kn--NoteWidget，
    // 这里统一改用 objectName 的 ID 选择器，避免命名空间转义问题
    return QStringLiteral(
               "#NoteWidget{background:%1;border:1px solid %3;border-radius:6px;}"
               "#TitleBar{background:%2;border-bottom:1px solid %3;}"
               "#NoteWidget QLabel{background:transparent;color:%4;}"
               "#NoteSchedule,#NoteStatus{color:%6;font-size:11px;}"
               "#NoteWidget QTextBrowser,#NoteChecklist{background:transparent;color:%4;border:none;}"
               "#NoteWidget #DescriptionEditor{selection-background-color:%5;}"
               "#TitleBar QToolButton{background:transparent;border:none;color:%6;padding:4px "
               "6px;margin:0px;}"
               "#TitleBar QToolButton:hover{background:rgba(0,0,0,0.08);border-radius:3px;}"
               "#TitleBar QToolButton:pressed{background:rgba(0,0,0,0.14);border-radius:3px;}")
        .arg(QLatin1String(s.body),
             QLatin1String(s.bar),
             QLatin1String(s.border),
             QLatin1String(s.text),
             QLatin1String(s.selection),
             QLatin1String(s.barText));
}

void ColorTheme::apply(QWidget* window, NoteColor c) {
    if (window) {
        window->setStyleSheet(windowStyle(c));
    }
}

}  // namespace kn
