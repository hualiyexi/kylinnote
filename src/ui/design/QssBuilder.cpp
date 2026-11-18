#include "QssBuilder.h"

namespace kl {
namespace ds {
namespace {

QString hex(ColorToken token, Theme theme) {
    return color(token, theme).name();
}

QString rgba(ColorToken token, Theme theme) {
    const QColor value = color(token, theme);
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(value.red()).arg(value.green()).arg(value.blue()).arg(value.alpha());
}

}  // namespace

QString buildStyleSheet(Theme theme) {
    const QString text = hex(ColorToken::Text, theme);
    const QString muted = hex(ColorToken::TextSecondary, theme);
    const QString faint = hex(ColorToken::TextTertiary, theme);
    const QString primary = hex(ColorToken::Primary, theme);
    const QString primaryHover = hex(ColorToken::PrimaryHover, theme);
    const QString onPrimary = hex(ColorToken::OnPrimary, theme);
    const QString selection = hex(ColorToken::Selection, theme);
    const QString surface = hex(ColorToken::Surface, theme);
    const QString sidebar = hex(ColorToken::Sidebar, theme);
    const QString railBg = hex(ColorToken::RailBg, theme);
    const QString sidebarHover = hex(ColorToken::SidebarHover, theme);
    const QString separator = hex(ColorToken::Divider, theme);
    const QString hover = hex(ColorToken::Hover, theme);
    const QString field = hex(ColorToken::Field, theme);
    const QString quickAddBg = hex(ColorToken::QuickAddBg, theme);
    const QString fieldHover = hex(ColorToken::FieldHover, theme);
    const QString border = hex(ColorToken::Border, theme);
    const QString menuBg = hex(ColorToken::MenuBg, theme);
    const QString menuHover = hex(ColorToken::MenuHover, theme);
    const QString menuDisabled = hex(ColorToken::MenuDisabled, theme);
    const QString danger = hex(ColorToken::Danger, theme);
    const QString dangerBg = hex(ColorToken::DangerBg, theme);
    const QString scrollbar = hex(ColorToken::Scrollbar, theme);
    const QString scrollbarHover = hex(ColorToken::ScrollbarHover, theme);
    const QString tooltipBg = rgba(ColorToken::TooltipBg, theme);
    const QString tooltipText = hex(ColorToken::TooltipText, theme);

    const QString menuFontSize = QStringLiteral("%1pt")
        .arg(QString::number(pointSize(TypeScale::Menu), 'f', 2));

    return QStringLiteral(
        "QMenu{font-size:%27;}"
        "QToolTip{color:%26;background:%2;border:0;padding:3px 8px;border-radius:6px;font-size:13px;}"
        "QLabel[muted=\"true\"]{color:%4;}"
        "QLabel[section=\"true\"]{color:%5;font-weight:600;padding-top:10px;}"
        "QPushButton,QToolButton{background:transparent;border:1px solid transparent;"
        "border-radius:8px;padding:5px 12px;color:%1;}"
        "QPushButton:hover,QToolButton:hover{background:%6;}"
        "QPushButton:pressed,QToolButton:pressed{background:%7;}"
        "QPushButton:focus,QToolButton:focus{border:1px solid transparent;background:%6;}"
        "QPushButton:disabled,QToolButton:disabled{color:%9;}"
        "QPushButton[primary=\"true\"]{background:%8;border:1px solid %8;color:%10;}"
        "QPushButton[primary=\"true\"]:hover{background:%11;border-color:%11;}"
        "QPushButton[primary=\"true\"]:pressed{background:%11;border-color:%11;}"
        "QPushButton[primary=\"true\"]:disabled{background:%13;border-color:%13;color:%9;}"
        "QPushButton[danger=\"true\"]{background:%21;border:1px solid %21;color:%10;}"
        "QPushButton[danger=\"true\"]:disabled{background:%20;border-color:%20;color:%9;}"
        "QPushButton[quiet=\"true\"]{color:%5;text-align:left;padding:7px 10px;}"
        "QPushButton[quiet=\"true\"]:hover{background:%12;color:%1;}"
        "QLineEdit,QDateTimeEdit,QComboBox,QSpinBox{background:%13;border:1px solid transparent;"
        "border-radius:8px;padding:6px 10px;selection-background-color:%14;selection-color:%1;}"
        "QLineEdit:hover,QDateTimeEdit:hover,QComboBox:hover,QSpinBox:hover{background:%15;}"
        "QLineEdit:focus,QDateTimeEdit:focus,QComboBox:focus,QSpinBox:focus{background:%16;"
        "border:1px solid transparent;}"
        "QLineEdit[invalid=\"true\"]{border:1px solid %21;}"
        "QComboBox::drop-down,QDateTimeEdit::drop-down{border:0;width:22px;}"
        "QComboBox QAbstractItemView{background:%17;border:1px solid %3;"
        "border-radius:6px;padding:4px;selection-background-color:%14;selection-color:%1;outline:0;}"
        "QTextEdit,QTextBrowser,QPlainTextEdit{background:%13;border:1px solid transparent;"
        "border-radius:6px;padding:8px;selection-background-color:%14;selection-color:%1;}"
        "QTextEdit:focus,QPlainTextEdit:focus{background:%16;border:1px solid transparent;}"
        "QCheckBox{spacing:8px;background:transparent;color:%1;}"
        "QCheckBox::indicator{width:16px;height:16px;}"
        "QListView#TaskList{border:0;background:%16;outline:0;}"
        "QListWidget#Navigation{border:0;background:transparent;outline:0;}"
        "QFrame#Sidebar{background:%18;border-right:1px solid %19;}"
        "QFrame#ListPane{background:%16;}"
        "QFrame#DetailFrame{background:%16;border-left:1px solid %19;}"
        "QLabel#ErrorBanner{background:%20;color:%21;border:0;border-radius:6px;padding:10px 14px;}"
        "QLabel#DetailStatus{color:%4;padding:2px 0;}"
        "QMenu{background:%17;border:1px solid %3;border-radius:12px;padding:4px 0;}"
        "QMenu::item{padding:7px 26px 7px 14px;border-radius:8px;color:%1;background:transparent;"
        "min-height:20px;}"
        "QMenu::item:selected{background:%22;color:%1;}"
        "QMenu::item:disabled{color:%23;}"
        "QMenu::separator{height:1px;background:%19;margin:5px 10px;}"
        "QMenu[entityMenu=\"true\"]::item{margin:0 4px;padding:7px 12px 7px 18px;height:20px;min-height:20px;border-radius:8px;}"
        "QMenu[entityMenu=\"true\"]::separator{height:1px;background:%19;margin:4px 0;}"
        "QMenu[entityMenu=\"true\"]::right-arrow{width:12px;height:12px;}"
        "QStatusBar{background:%18;border-top:1px solid %19;color:%4;}"
        "QStatusBar::item{border:0;}"
        "QSplitter::handle{background:transparent;}"
        "QSplitter::handle:hover{background:%14;}"
        "QScrollBar:vertical{background:transparent;width:6px;margin:2px 0;}"
        "QScrollBar::handle:vertical{background:%24;border-radius:3px;min-height:28px;}"
        "QScrollBar::handle:vertical:hover{background:%25;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;width:0;}"
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}"
        "QScrollBar:horizontal{background:transparent;height:6px;margin:0 2px;}"
        "QScrollBar::handle:horizontal{background:%24;border-radius:3px;min-width:28px;}"
        "QScrollBar::handle:horizontal:hover{background:%25;}"
        "QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal{height:0;width:0;}"
        "QScrollBar::add-page:horizontal,QScrollBar::sub-page:horizontal{background:transparent;}"
        "QScrollArea{border:0;background:transparent;}"
        "QTabBar::tab{background:transparent;color:%4;padding:6px 12px;border:0;"
        "border-bottom:2px solid transparent;}"
        "QTabBar::tab:selected{color:%8;border-bottom-color:%8;}"
        "QTabBar::tab:hover{color:%1;}"
        "QTabWidget::pane{border:0;}")
        .arg(text)          // %1
        .arg(tooltipBg)     // %2
        .arg(border)        // %3
        .arg(muted)         // %4
        .arg(muted)         // %5 (section)
        .arg(hover)         // %6
        .arg(fieldHover)    // %7 (pressed)
        .arg(primary)       // %8
        .arg(menuDisabled)  // %9
        .arg(onPrimary)     // %10
        .arg(primaryHover)  // %11
        .arg(sidebarHover)  // %12
        .arg(field)         // %13
        .arg(selection)     // %14
        .arg(fieldHover)    // %15
        .arg(surface)       // %16
        .arg(menuBg)        // %17
        .arg(sidebar)       // %18
        .arg(separator)     // %19
        .arg(dangerBg)      // %20
        .arg(danger)        // %21
        .arg(menuHover)     // %22
        .arg(menuDisabled)  // %23
        .arg(scrollbar)     // %24
        .arg(scrollbarHover)// %25
        .arg(tooltipText)   // %26
        .arg(menuFontSize)  // %27
        + QStringLiteral(
            "QWidget#TaskDetailHost,QWidget#TaskDetailForm,QScrollArea#DetailScroll,"
            "QScrollArea#DetailScroll>QWidget{background:%1;}"
            "QScrollArea#DetailScroll>QWidget#qt_scrollarea_viewport{background:transparent;}"
            "QFrame#DetailHeader{background:%1;border:0;border-bottom:1px solid %2;}"
            "QFrame#DetailFooter{background:%1;border:0;border-bottom-right-radius:10px;}"
            "QFrame#DetailFrame{background:%1;border:0;}"
            "QSplitter#WorkspaceSplitter::handle{background:%2;}"
            "QSplitter#WorkspaceSplitter::handle:hover{background:%5;}"
            "QLineEdit#TaskTitle,QLineEdit#TaskTitle:hover,QLineEdit#TaskTitle:focus{"
            "background:transparent;border:0;border-radius:0;padding:0;color:%6;}"
            "QWidget#TaskDetailForm QTextEdit#DescriptionEditor,"
            "QWidget#TaskDetailForm QTextEdit#DescriptionEditor:focus{"
            "background:transparent;border:0;border-radius:0;padding:0;}"
            "QPushButton#DateChip{background:transparent;border:0;padding:0 4px;"
            "text-align:left;color:%3;}"
            "QPushButton#DateChip:hover,QPushButton#DateChip:focus{background:%4;}"
            "QPushButton#DateChip[hasSchedule=\"true\"]{color:%INFO%;}"
            "QPushButton#DateChip[overdue=\"true\"]{color:%7;}"
            "QToolButton#DetailInfo,QToolButton#DetailPriority,QToolButton#DetailClose{padding:0;border:0;}"
            "QFrame#DetailHeaderSeparator,QFrame#DetailDateSeparator{background:%2;border:0;}"
            "QPushButton#ParentTaskLink{color:%3;background:transparent;border:0;padding:0;text-align:left;}"
            "QPushButton#ParentTaskLink:hover{color:%6;background:transparent;}"
            "QToolButton#ParentTaskArrow{padding:0;border:0;background:transparent;}"
            "QToolButton#ParentTaskArrow:hover{background:transparent;}"
            "QToolButton#GroupChip{padding:0;border:0;}"
            "QWidget#DetailRecoveryActions QToolButton{padding:2px 4px;color:%3;}"
            "QToolButton#FooterFormat,QToolButton#DetailMore,QToolButton#DetailAttachmentAdd{"
            "padding:0;border:0;}"
            "QToolButton#FooterFormat:checked{background:%4;}"
            "QLabel#AttachmentDropHint{background:%PRIMARY_SOFT%;color:%PRIMARY%;"
            "border:1px dashed %PRIMARY%;border-radius:8px;padding:12px;}"
            "QWidget#AttachmentRow[inlineAttachment=\"true\"]{background:%ATTACHMENT_OTHER%;"
            "border:1px solid transparent;border-radius:14px;}"
            "QWidget#AttachmentRow[inlineAttachment=\"true\"][attachmentType=\"document\"],"
            "QWidget#AttachmentRow[inlineAttachment=\"true\"][attachmentType=\"image\"]{background:%ATTACHMENT_DOCUMENT%;}"
            "QWidget#AttachmentRow[inlineAttachment=\"true\"][attachmentType=\"spreadsheet\"]{background:%ATTACHMENT_SPREADSHEET%;}"
            "QWidget#AttachmentRow[inlineAttachment=\"true\"][attachmentType=\"text\"]{background:%ATTACHMENT_TEXT%;}"
            "QWidget#AttachmentRow[inlineAttachment=\"true\"][attachmentType=\"pdf\"]{background:%ATTACHMENT_PDF%;}"
            "QWidget#AttachmentRow[inlineAttachment=\"true\"][attachmentType=\"presentation\"],"
            "QWidget#AttachmentRow[inlineAttachment=\"true\"][attachmentType=\"archive\"]{background:%ATTACHMENT_WARM%;}"
            "QWidget#AttachmentRow[inlineAttachment=\"true\"]:focus{border-color:%FOCUS_RING%;}"
            "QWidget#AttachmentRow[inlineAttachment=\"true\"] QLabel{background:transparent;border:0;padding:0;}"
            "QToolButton#AttachmentActions{min-width:24px;max-width:24px;"
            "min-height:24px;max-height:24px;padding:0;border:0;border-radius:6px;}"
            "QWidget#AttachmentRow[inlineAttachment=\"true\"] QToolButton#AttachmentActions{"
            "min-width:30px;max-width:30px;min-height:30px;max-height:30px;}"
            "QToolButton#AttachmentActions::menu-indicator{image:none;width:0;height:0;}"
            "QLabel#DetailStatus{color:%7;padding:6px 0;}"
            "QMainWindow#TaskPanel,QWidget#WorkspaceRoot{background:transparent;}"
            "QFrame#ActivityRail{background:%RAILBG%;border:0;border-right:1px solid %2;"
            "border-top-left-radius:10px;border-bottom-left-radius:10px;}"
            "QFrame#ActivityRail QToolButton{border:0;border-radius:8px;padding:0;}"
            "QFrame#ActivityRail QToolButton:hover,"
            "QFrame#ActivityRail QToolButton:pressed,"
            "QFrame#ActivityRail QToolButton:checked{background:transparent;}"
            "QToolButton#AccountButton{border-radius:9px;}"
            "QFrame#Sidebar{background:%8;border:0;}"
            "QFrame#ListPane,QFrame#ListHeader{background:%1;border:0;}"
            "QFrame#ListHeader QToolButton{padding:0;border:0;border-radius:6px;}"
            "QFrame#ListHeader QToolButton:hover{background:%4;}"
            "QFrame#ListHeader QToolButton:pressed{background:%5;}"
            "QFrame#DetailFrame{background:%1;border:0;}"
            "QWidget#TaskDetailWidget{background:transparent;}"
            "QFrame#QuickAddFrame{background:%QUICKADDBG%;border:0;border-radius:10px;}"
            "QFrame#QuickAddFrame:focus-within{background:%QUICKADDBG%;border:0;}"
            "QFrame#QuickAddFrame QLineEdit#QuickAdd,"
            "QFrame#QuickAddFrame QLineEdit#QuickAdd:hover,"
            "QFrame#QuickAddFrame QLineEdit#QuickAdd:focus{background:transparent;border:0;padding:0;}"
            "QLabel#QuickBarTitle{color:%FAINT%;padding:0;}"
            "QMenu[entityMenu=\"true\"] QWidget#dateQuickBarWidget QToolButton,"
            "QMenu[entityMenu=\"true\"] QWidget#priorityBarWidget QToolButton{"
            "padding:0;border:0;border-radius:8px;background:transparent;}"
            "QMenu[entityMenu=\"true\"] QWidget#dateQuickBarWidget QToolButton:hover,"
            "QMenu[entityMenu=\"true\"] QWidget#priorityBarWidget QToolButton:hover{background:%4;}"
            "QMenu[entityMenu=\"true\"] QWidget#dateQuickBarWidget QToolButton:pressed,"
            "QMenu[entityMenu=\"true\"] QWidget#priorityBarWidget QToolButton:pressed{background:%5;}"
            "QMenu[entityMenu=\"true\"] QWidget#priorityBarWidget QToolButton[quickPriority=\"true\"]:checked{"
            "background:%PRIMARY_SOFT%;}"
            "QToolButton#QuickAddDate{color:%3;padding:0 4px;border:0;}"
            "QToolButton#QuickAddDate[hasDate=\"true\"]{color:%PRIMARY%;}"
            "QWidget#WindowControls{background:transparent;border-top-right-radius:10px;}"
            "QWidget#WindowControls QToolButton{border:0;border-radius:0;padding:0;background:transparent;}"
            "QWidget#WindowControls QToolButton:hover,"
            "QWidget#WindowControls QToolButton:pressed{background:transparent;border:0;}"
            "QWidget#WindowDragArea{background:transparent;}"
            "QLabel#WorkspaceNotice{background:%9;color:%6;border:1px solid %2;"
            "border-radius:8px;padding:9px 14px;}"
            "QListView#TaskList{background:transparent;}"
            "QFrame#ToastIsland{background:transparent;border:0;}")
            .arg(surface).arg(hex(ColorToken::DetailDivider, theme)).arg(muted)
            .arg(hover).arg(selection).arg(text).arg(danger).arg(sidebar).arg(menuBg)
            // Qt 5 链式 arg 的双位数字占位易错位；组件专用色使用命名替换。
            .replace(QStringLiteral("%RAILBG%"), railBg)
            .replace(QStringLiteral("%QUICKADDBG%"), quickAddBg)
            .replace(QStringLiteral("%FAINT%"), faint)
            .replace(QStringLiteral("%FOCUS_RING%"), hex(ColorToken::FocusRing, theme))
            .replace(QStringLiteral("%ATTACHMENT_DOCUMENT%"), hex(ColorToken::AttachmentDocumentBg, theme))
            .replace(QStringLiteral("%ATTACHMENT_SPREADSHEET%"), hex(ColorToken::AttachmentSpreadsheetBg, theme))
            .replace(QStringLiteral("%ATTACHMENT_TEXT%"), hex(ColorToken::AttachmentTextBg, theme))
            .replace(QStringLiteral("%ATTACHMENT_OTHER%"), hex(ColorToken::AttachmentOtherBg, theme))
            .replace(QStringLiteral("%ATTACHMENT_PDF%"), hex(ColorToken::DangerBg, theme))
            .replace(QStringLiteral("%ATTACHMENT_WARM%"), hex(ColorToken::WarningBg, theme))
            .replace(QStringLiteral("%PRIMARY_SOFT%"), hex(ColorToken::PrimarySoft, theme))
            .replace(QStringLiteral("%PRIMARY%"), primary)
            .replace(QStringLiteral("%INFO%"), hex(ColorToken::Info, theme));
}

QString buildPreviewCss(Theme theme) {
    return QStringLiteral(
        "body{color:%1;font-size:10pt;} p{margin-top:6px;margin-bottom:8px;}"
        "h1{font-size:16pt;}h2{font-size:13pt;}h3{font-size:11pt;}"
        "pre,code{background-color:%2;}"
        "blockquote{color:%3;}a{color:%4;}a.trash-link{color:%3;text-decoration:line-through;}"
        ".broken-link{color:%5;background-color:%6;}.trash-marker{font-size:8pt;color:%3;}"
        "table{border-collapse:collapse;}td,th{border:1px solid %7;padding:5px;}")
        .arg(hex(ColorToken::Text, theme))
        .arg(hex(ColorToken::Field, theme))
        .arg(hex(ColorToken::TextSecondary, theme))
        .arg(hex(ColorToken::Primary, theme))
        .arg(hex(ColorToken::Danger, theme))
        .arg(hex(ColorToken::DangerBg, theme))
        .arg(hex(ColorToken::Divider, theme));
}

}  // namespace ds
}  // namespace kl
