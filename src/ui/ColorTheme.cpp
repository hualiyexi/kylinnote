#include "ColorTheme.h"

#include <QPalette>
#include <QWidget>

namespace kn {

ColorTheme::Spec ColorTheme::spec(NoteColor c) {
    switch (c) {
        case NoteColor::Yellow:
            return {"yellow", "奶油黄", "#FFF4BF", "#FFF4BF", "#F0D889", "#37342A", "#82734B", "#E5CF86"};
        case NoteColor::Blue:
            return {"blue", "天空蓝", "#E0EEF8", "#E0EEF8", "#BDD5E8", "#263640", "#5A7383", "#BAD5E9"};
        case NoteColor::Green:
            return {"green", "鼠尾草绿", "#E5F1DE", "#E5F1DE", "#C4DAB8", "#2C392B", "#657B5D", "#C2DAB4"};
        case NoteColor::Pink:
            return {"pink", "淡樱粉", "#F9E8EE", "#F9E8EE", "#E7C9D4", "#423039", "#8C6877", "#E7C8D6"};
        case NoteColor::Purple:
            return {"purple", "雾紫", "#EDE8F7", "#EDE8F7", "#D4C9E7", "#373043", "#776B8C", "#D2C6E5"};
        case NoteColor::White:
        default:
            return {"white", "纸白", "#FFFFFF", "#FFFFFF", "#E4E7ED", "#292D35", "#828A98", "#D5E0F5"};
    }
}

NoteColor ColorTheme::next(NoteColor c) {
    return fromInt((toInt(c) + 1) % ColorCount);
}

NoteColor ColorTheme::fromInt(int v) {
    return v >= 0 && v < ColorCount ? static_cast<NoteColor>(v) : NoteColor::Yellow;
}

int ColorTheme::toInt(NoteColor c) {
    return static_cast<int>(c);
}

QString ColorTheme::windowStyle(NoteColor c) {
    const Spec s = spec(c);
    return QStringLiteral(
               "#NoteWidget{background:transparent;border:0;}"
               "#TitleBar{background:transparent;border:0;}"
               "#NoteWidget QLabel{background:transparent;color:%4;}"
               "#NoteSchedule,#NoteStatus{color:%6;font-size:9pt;}"
               "#NoteWidget QTextBrowser,#NoteWidget QTextEdit{background:transparent;color:%4;border:0;}"
               "#NoteWidget #DescriptionEditor{selection-background-color:%5;padding:0;}"
               "#NoteEditButton{background:transparent;border:0;color:%6;padding:4px 6px;font-size:9pt;}"
               "#NoteEditButton:hover{color:%4;background:rgba(0,0,0,0.04);border-radius:4px;}"
               "#NoteWidget QCheckBox{background:transparent;border:0;spacing:0;}"
               "#NoteWidget QCheckBox::indicator{width:17px;height:17px;border:1.5px solid %6;"
               "border-radius:4px;background:transparent;}"
               "#NoteWidget QCheckBox::indicator:checked{background:%4;border-color:%4;}"
               "#TitleBar QToolButton{background:transparent;border:0;color:%6;padding:0;margin:0;}"
               "#TitleBar QToolButton:hover{background:rgba(0,0,0,0.05);border-radius:4px;}"
               "#TitleBar QToolButton:pressed{background:rgba(0,0,0,0.09);border-radius:4px;}"
               "#NoteWidget #EditorToolbar{background:transparent;border:0;}"
               "#NoteWidget #EditorToolbar QToolButton{border:0;background:transparent;color:%4;padding:3px;}"
               "#NoteWidget QTabBar{background:transparent;border:0;}"
               "#NoteWidget QTabBar::tab{background:transparent;color:%6;padding:3px 9px;border:0;}"
               "#NoteWidget QTabBar::tab:selected{color:%4;border-bottom:1px solid %4;}"
               "#NoteWidget QScrollBar:vertical{background:transparent;width:5px;}"
               "#NoteWidget QScrollBar::handle:vertical{background:%3;border-radius:2px;}"
               "#NoteWidget QScrollBar::add-line:vertical,#NoteWidget "
               "QScrollBar::sub-line:vertical{height:0;}")
        .arg(QLatin1String(s.border),
             QLatin1String(s.text),
             QLatin1String(s.selection),
             QLatin1String(s.barText));
}

void ColorTheme::apply(QWidget* window, NoteColor c) {
    if (!window)
        return;
    window->setStyleSheet(windowStyle(c));
    QPalette palette = window->palette();
    palette.setColor(QPalette::Window, QColor(spec(c).body));
    palette.setColor(QPalette::Base, QColor(spec(c).body));
    palette.setColor(QPalette::Text, QColor(spec(c).text));
    palette.setColor(QPalette::WindowText, QColor(spec(c).text));
    window->setPalette(palette);
}

}  // namespace kn
