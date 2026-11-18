#include "ColorTheme.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

#include <QPalette>
#include <QWidget>

namespace kl {

ColorTheme::Spec ColorTheme::spec(NoteColor color) {
    static const char* keys[] = {"yellow", "blue", "green", "pink", "purple", "white"};
    static const char* labels[] = {"奶油黄", "天空蓝", "鼠尾草绿", "淡樱粉", "雾紫", "纸白"};
    static const ds::ColorToken papers[] = {
        ds::ColorToken::NotePaperYellow, ds::ColorToken::NotePaperBlue, ds::ColorToken::NotePaperGreen,
        ds::ColorToken::NotePaperPink, ds::ColorToken::NotePaperPurple, ds::ColorToken::NotePaperWhite
    };
    const int index = toInt(fromInt(toInt(color)));
    const auto& theme = ThemeManager::instance();
    const QString paper = theme.color(papers[index]).name();
    QColor border = theme.color(ds::ColorToken::NoteBorder);
    border.setAlpha(110);
    return {keys[index], labels[index], paper, paper, border.name(QColor::HexArgb),
            theme.color(ds::ColorToken::NoteText).name(), theme.color(ds::ColorToken::NoteMuted).name(),
            theme.color(ds::ColorToken::Selection).name()};
}

NoteColor ColorTheme::next(NoteColor color) { return fromInt((toInt(color) + 1) % ColorCount); }
NoteColor ColorTheme::fromInt(int value) { return colorFromInt(value); }
int ColorTheme::toInt(NoteColor color) { return colorToInt(color); }

QString ColorTheme::windowStyle(NoteColor color) {
    const Spec s = spec(color);
    const auto& theme = ThemeManager::instance();
    const QString hover = theme.color(ds::ColorToken::SurfaceHover).name();
    return QStringLiteral(
        "QWidget#NoteWidget,QWidget#NotePaper,QWidget#TitleBar{background:transparent;border:0;}"
        "QWidget#NoteWidget QWidget#TaskDetailHost,QWidget#NoteWidget QWidget#TaskDetailForm,"
        "QWidget#NoteWidget QScrollArea#DetailScroll,QWidget#NoteWidget QScrollArea#DetailAttachmentScroll,"
        "QWidget#NoteWidget QWidget#DetailAttachmentPane,QWidget#NoteWidget QFrame#DetailFooter"
        "{background:transparent;border:0;}"
        "QWidget#NoteWidget QScrollArea>QWidget>QWidget{background:transparent;}"
        "QWidget#NoteWidget QLabel,QWidget#NoteWidget QLineEdit#TaskTitle,"
        "QWidget#NoteWidget QTextEdit#DescriptionEditor{background:transparent;color:%1;border:0;padding:0;}"
        "QWidget#NoteWidget QLineEdit#TaskTitle{font-size:%5pt;font-weight:600;}"
        "QWidget#NoteWidget QTextEdit#DescriptionEditor{selection-background-color:%3;}"
        "QWidget#NoteWidget QPushButton#DateChip{background:transparent;border:0;color:%2;"
        "padding:0;font-size:%6pt;font-weight:400;min-width:0;}"
        "QWidget#NoteWidget QPushButton#DateChip:hover{background:transparent;color:%1;}"
        "QWidget#NoteWidget QToolButton#DetailMore,QWidget#TitleBar QToolButton"
        "{background:transparent;border:0;color:%2;padding:0;margin:0;}"
        "QWidget#NoteWidget QToolButton#DetailMore:hover,QWidget#TitleBar QToolButton:hover"
        "{background:%4;border-radius:3px;}"
        "QWidget#NoteWidget QScrollBar:vertical{background:transparent;width:5px;}"
        "QWidget#NoteWidget QScrollBar::handle:vertical{background:%2;border-radius:2px;min-height:18px;}"
        "QWidget#NoteWidget QScrollBar::add-line:vertical,QWidget#NoteWidget QScrollBar::sub-line:vertical"
        "{height:0;}"
        "QWidget#NoteWidget QLabel#NoteStatus{background:transparent;color:%7;font-size:%6pt;}"
    ).arg(s.text, s.barText, s.selection, hover)
     .arg(ds::qfont(ds::FontScene::ListSection).pointSizeF())
     .arg(ds::qfont(ds::FontScene::Caption).pointSizeF())
     .arg(theme.color(ds::ColorToken::Danger).name());
}

void ColorTheme::apply(QWidget* window, NoteColor color) {
    if (!window) return;
    const Spec s = spec(color);
    window->setStyleSheet(windowStyle(color));
    QPalette palette = window->palette();
    palette.setColor(QPalette::Window, QColor(s.body));
    palette.setColor(QPalette::Base, QColor(s.body));
    palette.setColor(QPalette::Text, QColor(s.text));
    palette.setColor(QPalette::WindowText, QColor(s.text));
    window->setPalette(palette);
}

} // namespace kl
