#include "AppStyle.h"
#include <QApplication>
#include <QFontDatabase>
#include <QPalette>

namespace kn {
namespace AppStyle {
namespace {
QString family() {
    static const QString resolved = [] {
        const QStringList available = QFontDatabase().families();
        for (const QString& name : {QStringLiteral("Microsoft YaHei UI"),
                                    QStringLiteral("Microsoft YaHei"),
                                    QStringLiteral("Noto Sans CJK SC"),
                                    QStringLiteral("WenQuanYi Micro Hei")})
            if (available.contains(name))
                return name;
        return QApplication::font().family();
    }();
    return resolved;
}
QString hex(Color role) {
    return color(role).name();
}
QString sheet() {
    // 全部规则引用同一组令牌，保证三栏、详情与弹层视觉一致。
    return QStringLiteral(
               "QToolTip{color:%1;background:#FFFFFF;border:1px solid #E4E8F0;padding:6px 8px;"
               "border-radius:4px;}"
               "QLabel[muted=\"true\"]{color:%2;}"
               "QLabel[section=\"true\"]{color:#6B7484;font-weight:600;padding-top:10px;}"
               "QPushButton,QToolButton{background:transparent;border:1px solid transparent;"
               "border-radius:%10px;padding:5px 12px;color:%1;}"
               "QPushButton:hover,QToolButton:hover{background:%13;}"
               "QPushButton:pressed,QToolButton:pressed{background:#E2E7F1;}"
               "QPushButton:focus,QToolButton:focus{border:1px solid %3;}"
               "QPushButton:disabled,QToolButton:disabled{color:#B4BAC5;}"
               "QPushButton[primary=\"true\"]{background:%3;border:1px solid %3;color:%5;}"
               "QPushButton[primary=\"true\"]:hover{background:%4;border-color:%4;}"
               "QPushButton[primary=\"true\"]:pressed{background:%4;border-color:%4;}"
               "QPushButton[quiet=\"true\"]{color:#6B7484;text-align:left;padding:7px 10px;}"
               "QPushButton[quiet=\"true\"]:hover{background:%8;color:%1;}"
               "QLineEdit,QDateTimeEdit,QComboBox,QSpinBox{background:%11;border:1px solid transparent;"
               "border-radius:%10px;padding:6px 10px;selection-background-color:%6;selection-color:%1;}"
               "QLineEdit:hover,QDateTimeEdit:hover,QComboBox:hover,QSpinBox:hover{background:%12;}"
               "QLineEdit:focus,QDateTimeEdit:focus,QComboBox:focus,QSpinBox:focus{background:#FFFFFF;"
               "border:1px solid %3;}"
               "QComboBox::drop-down,QDateTimeEdit::drop-down{border:0;width:22px;}"
               "QComboBox QAbstractItemView{background:#FFFFFF;border:1px solid #E4E8F0;"
               "border-radius:6px;padding:4px;selection-background-color:%6;selection-color:%1;outline:0;}"
               "QComboBox#TaskSort{background:transparent;color:#5B6472;}"
               "QComboBox#TaskSort:hover{background:%11;}"
               "QComboBox#TaskSort:focus{background:%11;}"
               "QTextEdit,QTextBrowser,QPlainTextEdit{background:#F7F8FB;border:1px solid transparent;"
               "border-radius:%10px;padding:8px;selection-background-color:#D9E4FF;selection-color:%1;}"
               "QTextEdit:focus,QPlainTextEdit:focus{background:#FFFFFF;border:1px solid %3;}"
               "QCheckBox{spacing:8px;background:transparent;}"
               "QCheckBox::indicator{width:16px;height:16px;}"
               "QListView#TaskList{border:0;background:%7;outline:0;}"
               "QListWidget#Navigation{border:0;background:transparent;outline:0;}"
               "QListWidget#TaskChecklist{background:#F7F8FB;border:1px solid transparent;"
               "border-radius:6px;padding:4px;}"
               "QListWidget#TaskChecklist::item{padding:4px 6px;}"
               "QFrame#Sidebar{background:%9;border-right:1px solid %14;}"
               "QFrame#ListPane{background:%7;}"
               "QFrame#DetailFrame{background:%7;border-left:1px solid %14;}"
               "QFrame#EditorToolbar{background:transparent;border:0;}"
               "QFrame#EditorToolbar QToolButton{padding:5px 8px;color:#5B6472;}"
               "QFrame#EditorToolbar QToolButton:hover{background:%12;color:%1;}"
               "QLabel#ErrorBanner{background:#FBF0EE;color:#B5463B;border:0;border-radius:6px;"
               "padding:10px 14px;}"
               "QLabel#DetailStatus{color:%2;padding:2px 0;}"
               "QMenu{background:#FFFFFF;border:1px solid #E4E8F0;border-radius:8px;padding:5px;}"
               "QMenu::item{padding:7px 26px 7px 14px;border-radius:4px;color:%1;background:transparent;}"
               "QMenu::item:selected{background:%6;color:%1;}"
               "QMenu::item:disabled{color:#B4BAC5;}"
               "QMenu::separator{height:1px;background:%14;margin:5px 10px;}"
               "QStatusBar{background:#FAFBFD;border-top:1px solid %14;color:%2;}"
               "QStatusBar::item{border:0;}"
               "QSplitter::handle{background:transparent;}"
               "QSplitter::handle:hover{background:#E3EAF7;}"
               "QScrollBar:vertical{background:transparent;width:6px;margin:2px 0;}"
               "QScrollBar::handle:vertical{background:#D9DFEA;border-radius:3px;min-height:28px;}"
               "QScrollBar::handle:vertical:hover{background:#C2CCDD;}"
               "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;width:0;}"
               "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}"
               "QScrollBar:horizontal{background:transparent;height:6px;margin:0 2px;}"
               "QScrollBar::handle:horizontal{background:#D9DFEA;border-radius:3px;min-width:28px;}"
               "QScrollBar::handle:horizontal:hover{background:#C2CCDD;}"
               "QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal{height:0;width:0;}"
               "QScrollBar::add-page:horizontal,QScrollBar::sub-page:horizontal{background:transparent;}"
               "QScrollArea{border:0;background:transparent;}"
               "QTabBar::tab{background:transparent;color:%2;padding:6px 12px;border:0;"
               "border-bottom:2px solid transparent;}"
               "QTabBar::tab:selected{color:%3;border-bottom-color:%3;}"
               "QTabBar::tab:hover{color:%1;}"
               "QTabWidget::pane{border:0;}")
        .arg(hex(Color::Text))        // %1
        .arg(hex(Color::Muted))       // %2
        .arg(hex(Color::Accent))      // %3
        .arg(hex(Color::AccentDark))  // %4
        .arg(hex(Color::OnAccent))    // %5
        .arg(hex(Color::Selection))   // %6
        .arg(hex(Color::Surface))     // %7
        .arg(hex(Color::SidebarHover))// %8
        .arg(hex(Color::Sidebar))     // %9
        .arg(metric(Metric::RadiusControl))  // %10
        .arg(hex(Color::Field))       // %11
        .arg(hex(Color::FieldHover))  // %12
        .arg(hex(Color::Hover))       // %13
        .arg(hex(Color::Separator));  // %14
}
}  // namespace

const QColor& color(Color role) {
    static const QColor values[] = {
        QColor(QStringLiteral("#20242C")),  // Text
        QColor(QStringLiteral("#858C99")),  // Muted
        QColor(QStringLiteral("#AEB5C0")),  // Faint
        QColor(QStringLiteral("#5279E8")),  // Accent
        QColor(QStringLiteral("#4165CE")),  // AccentDark
        QColor(QStringLiteral("#FFFFFF")),  // OnAccent
        QColor(QStringLiteral("#EAF0FF")),  // Selection
        QColor(QStringLiteral("#F5F7FC")),  // Sidebar
        QColor(QStringLiteral("#EBEFF8")),  // SidebarHover
        QColor(QStringLiteral("#FFFFFF")),  // Surface
        QColor(QStringLiteral("#EDF0F5")),  // Separator
        QColor(QStringLiteral("#F5F7FB")),  // Hover
        QColor(QStringLiteral("#F2F4F8")),  // Field
        QColor(QStringLiteral("#ECEFF5")),  // FieldHover
        QColor(QStringLiteral("#D9534F")),  // Danger
        QColor(QStringLiteral("#E2A63D")),  // Warning
        QColor(QStringLiteral("#E5A83C"))   // Star
    };
    return values[static_cast<int>(role)];
}
int metric(Metric role) {
    switch (role) {
        case Metric::ContentMargin:
            return 24;
        case Metric::SidebarWidth:
            return 208;
        case Metric::ControlHeight:
            return 36;
        case Metric::TaskRowHeight:
            return 54;
        case Metric::NavRowHeight:
            return 32;
        case Metric::RadiusRow:
            return 4;
        case Metric::RadiusControl:
            return 6;
        case Metric::Hairline:
            return 1;
    }
    return 0;
}
QFont font(Font role) {
    QFont f(family());
    switch (role) {
        case Font::Body:
            f.setPointSizeF(9.5);
            break;
        case Font::Small:
            f.setPointSizeF(8.5);
            break;
        case Font::Heading:
            f.setPointSizeF(20.0);
            f.setWeight(QFont::DemiBold);
            break;
        case Font::Brand:
            f.setPointSizeF(11.0);
            f.setWeight(QFont::DemiBold);
            break;
    }
    return f;
}
void apply(QApplication& app) {
    app.setStyle(QStringLiteral("Fusion"));
    app.setFont(font(Font::Body));
    QPalette palette;
    palette.setColor(QPalette::Window, color(Color::Surface));
    palette.setColor(QPalette::WindowText, color(Color::Text));
    palette.setColor(QPalette::Base, color(Color::Surface));
    palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#F7F8FB")));
    palette.setColor(QPalette::Text, color(Color::Text));
    palette.setColor(QPalette::Button, color(Color::Field));
    palette.setColor(QPalette::ButtonText, color(Color::Text));
    palette.setColor(QPalette::Highlight, color(Color::Accent));
    palette.setColor(QPalette::HighlightedText, color(Color::OnAccent));
    palette.setColor(QPalette::ToolTipBase, color(Color::Surface));
    palette.setColor(QPalette::ToolTipText, color(Color::Text));
    palette.setColor(QPalette::Link, color(Color::Accent));
    palette.setColor(QPalette::PlaceholderText, color(Color::Faint));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#B4BAC5")));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#B4BAC5")));
    app.setPalette(palette);
    app.setStyleSheet(sheet());
}
QString previewCss() {
    return QStringLiteral(
               "body{color:%1;font-size:10pt;} p{margin-top:6px;margin-bottom:8px;}"
               "h1{font-size:16pt;}h2{font-size:13pt;}h3{font-size:11pt;}"
               "pre,code{background-color:%2;}"
               "blockquote{color:%3;}a{color:%4;}"
               "table{border-collapse:collapse;}td,th{border:1px solid %5;padding:5px;}")
        .arg(hex(Color::Text))
        .arg(hex(Color::Field))
        .arg(hex(Color::Muted))
        .arg(hex(Color::Accent))
        .arg(hex(Color::Separator));
}
}  // namespace AppStyle
}  // namespace kn
