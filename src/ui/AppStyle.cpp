#include "AppStyle.h"
#include <QApplication>
#include <QFontDatabase>
#include <QPalette>

namespace kn {
void AppStyle::apply(QApplication& app) {
    app.setStyle(QStringLiteral("Fusion"));
    QFont font = app.font();
    const QStringList available = QFontDatabase().families();
    for (const QString& family : {QStringLiteral("Microsoft YaHei"),
                                  QStringLiteral("Noto Sans CJK SC"),
                                  QStringLiteral("WenQuanYi Micro Hei")}) {
        if (available.contains(family)) {
            font.setFamily(family);
            break;
        }
    }
    font.setPointSize(10);
    app.setFont(font);
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#F4F6FA"));
    palette.setColor(QPalette::WindowText, QColor("#23324B"));
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::Text, QColor("#23324B"));
    palette.setColor(QPalette::Button, QColor("#FFFFFF"));
    palette.setColor(QPalette::ButtonText, QColor("#23324B"));
    palette.setColor(QPalette::Highlight, QColor("#315BA6"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#8B96A7"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#8B96A7"));
    app.setPalette(palette);
    app.setStyleSheet(QStringLiteral(
        "QToolTip{color:#23324B;background:#FFF;border:1px solid #B8C7DF;padding:5px;}"
        "QPushButton,QToolButton{padding:6px 10px;border:1px solid "
        "#D7DFEB;border-radius:5px;background:#FFF;}"
        "QPushButton:hover,QToolButton:hover{background:#EDF3FC;border-color:#A9BDDE;}"
        "QPushButton:pressed,QToolButton:pressed{background:#DEE9FA;}"
        "QPushButton:focus,QToolButton:focus{border:1px solid #315BA6;}"
        "QPushButton:disabled,QToolButton:disabled{color:#8994A5;background:#F1F3F6;}"
        "QPushButton[primary=\"true\"]{background:#315BA6;border-color:#315BA6;color:#FFF;}"
        "QPushButton[primary=\"true\"]:hover{background:#244B90;}"
        "QLineEdit,QDateTimeEdit,QComboBox{background:#FFF;border:1px solid "
        "#D7DFEB;border-radius:4px;padding:6px;}"
        "QLineEdit:focus,QDateTimeEdit:focus,QComboBox:focus{border-color:#315BA6;}"
        "QTextEdit,QTextBrowser{border:1px solid #DEE5EF;border-radius:4px;background:#FFF;padding:6px;}"
        "QCheckBox{spacing:7px;} QCheckBox::indicator{width:17px;height:17px;}"
        "QLabel[muted=\"true\"]{color:#64748B;}"
        "QLabel[section=\"true\"]{font-weight:600;padding-top:8px;}"
        "QFrame#Sidebar{background:#EBEFF6;border-right:1px solid #DDE4EF;}"
        "QListWidget#Navigation{border:0;background:transparent;outline:0;}"
        "QListWidget#Navigation::item{padding:10px 12px;border-radius:5px;margin:1px 0;color:#3C4C65;}"
        "QListWidget#Navigation::item:selected{background:#D8E4F7;color:#234C90;font-weight:600;}"
        "QListView#TaskList{border:0;background:#FFF;outline:0;}"
        "QFrame#DetailFrame{background:#FFF;border-left:1px solid #DFE6F0;}"
        "QFrame#EditorToolbar{background:#F2F5FA;border:1px solid #DEE5EF;border-radius:4px;}"
        "QFrame#EditorToolbar QToolButton{border:0;background:transparent;padding:5px 7px;}"
        "QLabel#ErrorBanner{background:#FFF0EE;color:#9C302B;border:1px solid #EAC2BC;padding:8px;}"
        "QLabel#DetailStatus{color:#64748B;padding:4px;}"
        "QMenu{background:#FFF;border:1px solid #CAD5E5;padding:5px;}"
        "QMenu::item{padding:7px 22px;}QMenu::item:selected{background:#E7EFFC;}"
        "QSplitter::handle{background:#E1E7F0;width:3px;}"
        "QScrollArea{border:0;background:transparent;}"
        "QTabBar::tab{padding:7px 15px;}"));
}

QString AppStyle::previewCss() {
    return QStringLiteral(
        "body{color:#23324B;font-size:11pt;} p{margin-top:6px;margin-bottom:8px;}"
        "h1{font-size:18pt;}h2{font-size:15pt;}h3{font-size:12pt;}"
        "pre,code{background-color:#EDF1F7;font-family:monospace;}"
        "blockquote{color:#64748B;}a{color:#315BA6;}"
        "table{border-collapse:collapse;}td,th{border:1px solid #CED7E5;padding:5px;}");
}
}  // namespace kn
