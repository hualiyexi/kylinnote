#include "AppIcons.h"
#include <QByteArray>
#include <QPainter>
#include <QSvgRenderer>
#include "design/ThemeManager.h"

namespace kl {
namespace AppIcons {
namespace {

// 单色线性 SVG 模板（24x24 视图框，stroke=currentColor 由着色替换）。
// 采用简洁几何，风格对齐考据的线性图标语言。
const char* svgSource(IconId id) {
    switch (id) {
        case IconId::Inbox:
            return "<path d='M3 8h18v11a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1z'/><path d='M3 8l2-4h14l2 4'/><path d='M3 13h5l1 2h6l1-2h5'/>";
        case IconId::Today:
            return "<rect x='4' y='5' width='16' height='16' rx='2'/><path d='M4 10h16'/><path d='M8 3v4M16 3v4'/><circle cx='12' cy='15' r='2' fill='currentColor' stroke='none'/>";
        case IconId::Upcoming:
            return "<rect x='4' y='5' width='16' height='16' rx='2'/><path d='M4 10h16'/><path d='M8 3v4M16 3v4'/><path d='M8 15h8'/>";
        case IconId::Overdue:
            return "<circle cx='12' cy='12' r='8'/><path d='M12 7v5l3 2'/>";
        case IconId::Done:
            return "<circle cx='12' cy='12' r='8'/><path d='M8 12l3 3 5-6'/>";
        case IconId::Abandoned:
            return "<circle cx='12' cy='12' r='8'/><path d='M6 18L18 6'/>";
        case IconId::Trash:
            return "<path d='M4 6h16'/><path d='M9 6V4h6v2'/><path d='M6 6l1 14h10l1-14'/><path d='M10 10v7M14 10v7'/>";
        case IconId::Folder:
            return "<path d='M3 7a1 1 0 0 1 1-1h5l2 2h9a1 1 0 0 1 1 1v10a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1z'/>";
        case IconId::Note:
            return "<rect x='5' y='3' width='14' height='18' rx='2'/><path d='M8 8h8M8 12h8M8 16h5'/>";
        case IconId::TaskList:
            return "<rect x='4' y='4' width='16' height='16' rx='2'/><path d='M7 9l1.5 1.5L11 8'/><path d='M13 9h4'/><path d='M7 15h10'/>";
        case IconId::Pin:
            return "<path d='M12 3l-3 3M12 3l3 3'/><rect x='7' y='6' width='10' height='4' rx='1'/><path d='M12 10v9'/><path d='M9 17h6'/>";
        case IconId::Search:
            return "<circle cx='11' cy='11' r='7'/><path d='M20 20l-4-4'/>";
        case IconId::More:
            return "<circle cx='5' cy='12' r='1.6' fill='currentColor' stroke='none'/><circle cx='12' cy='12' r='1.6' fill='currentColor' stroke='none'/><circle cx='19' cy='12' r='1.6' fill='currentColor' stroke='none'/>";
        case IconId::Add:
            return "<path d='M12 5v14M5 12h14'/>";
        case IconId::Close:
            return "<path d='M6 6l12 12M18 6L6 18'/>";
        case IconId::Check:
            return "<path d='M5 12l4 4L19 6'/>";
        case IconId::Calendar:
            return "<rect x='4' y='5' width='16' height='16' rx='2'/><path d='M4 10h16'/><path d='M8 3v4M16 3v4'/>";
        case IconId::Clock:
            return "<circle cx='12' cy='12' r='8'/><path d='M12 7v5l3 2'/>";
        case IconId::Reminder:
            return "<path d='M6 9a6 6 0 0 1 12 0c0 5 2 6 2 6H4s2-1 2-6'/><path d='M10 20a2 2 0 0 0 4 0'/>";
        case IconId::Repeat:
            return "<path d='M17 2l4 4-4 4'/><path d='M3 11V9a4 4 0 0 1 4-4h14'/><path d='M7 22l-4-4 4-4'/><path d='M21 13v2a4 4 0 0 1-4 4H3'/>";
        case IconId::FlagHigh:
        case IconId::FlagMedium:
        case IconId::FlagLow:
            return "<path d='M5 21V4'/><path d='M5 4h12l-2 4 2 4H5' fill='currentColor'/>";
        case IconId::FlagNone:
            return "<path d='M5 21V4'/><path d='M5 4h12l-2 4 2 4H5'/>";
        case IconId::Attachment:
            return "<path d='M21 11l-9 9a5 5 0 0 1-7-7l9-9a3.5 3.5 0 0 1 5 5l-9 9a2 2 0 0 1-3-3l8-8'/>";
        case IconId::Tag:
            return "<path d='M20 12l-8 8-9-9V3h8z'/><circle cx='7.5' cy='7.5' r='1.2' fill='currentColor' stroke='none'/>";
        case IconId::ActivityLog:
            return "<path d='M3 12a9 9 0 1 0 3-6.7'/><path d='M3 4v5h5'/><path d='M12 7v5l3 2'/>";
        case IconId::Convert:
            return "<path d='M17 2l4 4-4 4'/><path d='M3 11V9a4 4 0 0 1 4-4h14'/><path d='M7 22l-4-4 4-4'/><path d='M21 13v2a4 4 0 0 1-4 4H3'/>";
        case IconId::Restore:
            return "<path d='M3 12a9 9 0 1 0 3-6.7'/><path d='M3 4v5h5'/>";
        case IconId::Delete:
            return "<path d='M4 6h16'/><path d='M9 6V4h6v2'/><path d='M6 6l1 14h10l1-14'/>";
        case IconId::DesktopNote:
            return "<rect x='4' y='4' width='16' height='14' rx='2'/><path d='M9 21h6M12 18v3'/>";
        case IconId::Settings:
            return "<circle cx='12' cy='12' r='3'/><path d='M12 2v3M12 19v3M2 12h3M19 12h3M4.9 4.9l2.1 2.1M17 17l2.1 2.1M19.1 4.9L17 7M7 17l-2.1 2.1'/>";
        case IconId::Backup:
            return "<path d='M12 3v12'/><path d='M7 10l5 5 5-5'/><path d='M4 17v3a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-3'/>";
        case IconId::Theme:
            return "<circle cx='12' cy='12' r='8'/><path d='M12 4a8 8 0 0 0 0 16z' fill='currentColor' stroke='none'/>";
        case IconId::Bold:
            return "<path d='M7 4h6a4 4 0 0 1 0 8H7zM7 12h7a4 4 0 0 1 0 8H7z'/>";
        case IconId::Italic:
            return "<path d='M10 4h8M6 20h8M14 4l-4 16'/>";
        case IconId::Strike:
            return "<path d='M4 12h16'/><path d='M17 6a4 4 0 0 0-8-1'/><path d='M7 18a4 4 0 0 0 8 1'/>";
        case IconId::Heading:
            return "<path d='M5 5v14M19 5v14M5 12h14'/>";
        case IconId::List:
            return "<path d='M9 6h12M9 12h12M9 18h12'/><circle cx='4' cy='6' r='1' fill='currentColor' stroke='none'/><circle cx='4' cy='12' r='1' fill='currentColor' stroke='none'/><circle cx='4' cy='18' r='1' fill='currentColor' stroke='none'/>";
        case IconId::OrderedList:
            return "<path d='M10 6h11M10 12h11M10 18h11'/><path d='M4 5h1v3M4 12h2l-2 3h2'/>";
        case IconId::Quote:
            return "<path d='M4 5v14'/><path d='M9 8h10M9 12h10M9 16h6'/>";
        case IconId::Code:
            return "<path d='M8 6l-6 6 6 6M16 6l6 6-6 6'/>";
        case IconId::Checklist:
            return "<rect x='3' y='4' width='6' height='6' rx='1'/><path d='M4 7l1.5 1.5L8 6'/><path d='M13 7h8'/><rect x='3' y='14' width='6' height='6' rx='1'/><path d='M13 17h8'/>";
        case IconId::Link:
            return "<path d='M10 14a5 5 0 0 0 7 0l3-3a5 5 0 0 0-7-7l-1 1'/><path d='M14 10a5 5 0 0 0-7 0l-3 3a5 5 0 0 0 7 7l1-1'/>";
        case IconId::Underline:
            return "<path d='M6 4v8a6 6 0 0 0 12 0V4M4 21h16'/>";
        case IconId::Highlight:
            return "<path d='M9 14l7-10 4 3-7 10-4-3zM9 14l-3 5 4-1 3-1M4 21h12'/>";
        case IconId::Expand:
            return "<path d='M9 4H4v5M15 4h5v5M4 15v5h5M20 15v5h-5'/>";
        case IconId::Info:
            return "<circle cx='12' cy='12' r='8'/><path d='M12 11v6'/><circle cx='12' cy='7.5' r='.7' fill='currentColor' stroke='none'/>";
        case IconId::ChevronDown:
            return "<path d='M7 10l5 5 5-5'/>";
        case IconId::FormatClear:
            return "<path d='M4 5h14M11 5l-4 14M15 14l6 6M21 14l-6 6'/>";
        case IconId::Format:
            return "<path d='M5 17L12 3l7 14M8 12h8M4 21h16'/>";
        case IconId::Comment:
            return "<path d='M6 4h12a3 3 0 0 1 3 3v8a3 3 0 0 1-3 3h-7l-5 3v-3a3 3 0 0 1-3-3V7a3 3 0 0 1 3-3zM7 9h10M7 13h7'/>";
        default:
            return "";
    }
}

QByteArray coloredSvg(IconId id, const QColor& color) {
    const char* paths = svgSource(id);
    QByteArray svg;
    svg.reserve(512);
    svg.append("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' "
               "fill='none' stroke='");
    svg.append(color.name().toLatin1());
    svg.append("' stroke-width='1.6' stroke-linecap='round' stroke-linejoin='round'>");
    // currentColor 引用替换为实际颜色（fill 用）。
    QByteArray p(paths);
    p.replace("currentColor", color.name().toLatin1());
    svg.append(p);
    svg.append("</svg>");
    return svg;
}

QPixmap renderPixmap(IconId id, const QColor& color, int size, qreal dpr) {
    QPixmap pixmap(qRound(size * dpr), qRound(size * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QSvgRenderer renderer(coloredSvg(id, color));
    QPainter painter(&pixmap);
    renderer.render(&painter, QRectF(0, 0, size, size));
    return pixmap;
}

}  // namespace

QIcon icon(IconId id, int size) {
    return icon(id, ds::ColorToken::TextSecondary, size);
}

QIcon icon(IconId id, ds::ColorToken colorToken, int size) {
    if (id == IconId::None) return QIcon();
    const QColor color = ThemeManager::instance().color(colorToken);
    const qreal dpr = 2.0;  // 高 DPI 超采样
    QIcon result;
    result.addPixmap(renderPixmap(id, color, size, dpr));
    return result;
}

void paint(QPainter* painter, IconId id, const QRectF& rect, const QColor& color) {
    if (id == IconId::None) return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    QSvgRenderer renderer(coloredSvg(id, color));
    renderer.render(painter, rect);
    painter->restore();
}

QString accessibleName(IconId id) {
    switch (id) {
        case IconId::Inbox: return QStringLiteral("收集箱");
        case IconId::Today: return QStringLiteral("今天");
        case IconId::Upcoming: return QStringLiteral("七天内");
        case IconId::Overdue: return QStringLiteral("已逾期");
        case IconId::Done: return QStringLiteral("已完成");
        case IconId::Abandoned: return QStringLiteral("已放弃");
        case IconId::Trash: return QStringLiteral("回收站");
        case IconId::Folder: return QStringLiteral("文件夹");
        case IconId::Note: return QStringLiteral("笔记");
        case IconId::TaskList: return QStringLiteral("任务清单");
        case IconId::Pin: return QStringLiteral("置顶");
        case IconId::Search: return QStringLiteral("搜索");
        case IconId::More: return QStringLiteral("更多");
        case IconId::Add: return QStringLiteral("添加");
        case IconId::Close: return QStringLiteral("关闭");
        case IconId::Check: return QStringLiteral("完成");
        case IconId::Calendar: return QStringLiteral("日期");
        case IconId::Clock: return QStringLiteral("时间");
        case IconId::Reminder: return QStringLiteral("提醒");
        case IconId::Repeat: return QStringLiteral("重复");
        case IconId::FlagHigh: return QStringLiteral("高优先级");
        case IconId::FlagMedium: return QStringLiteral("中优先级");
        case IconId::FlagLow: return QStringLiteral("低优先级");
        case IconId::FlagNone: return QStringLiteral("无优先级");
        case IconId::Attachment: return QStringLiteral("附件");
        case IconId::Tag: return QStringLiteral("标签");
        case IconId::ActivityLog: return QStringLiteral("活动日志");
        case IconId::Convert: return QStringLiteral("类型转换");
        case IconId::Restore: return QStringLiteral("恢复");
        case IconId::Delete: return QStringLiteral("删除");
        case IconId::DesktopNote: return QStringLiteral("桌面便签");
        case IconId::Settings: return QStringLiteral("设置");
        case IconId::Backup: return QStringLiteral("备份");
        case IconId::Theme: return QStringLiteral("主题");
        case IconId::Bold: return QStringLiteral("加粗");
        case IconId::Italic: return QStringLiteral("斜体");
        case IconId::Strike: return QStringLiteral("删除线");
        case IconId::Heading: return QStringLiteral("标题");
        case IconId::List: return QStringLiteral("列表");
        case IconId::OrderedList: return QStringLiteral("编号");
        case IconId::Quote: return QStringLiteral("块引用");
        case IconId::Code: return QStringLiteral("代码");
        case IconId::Checklist: return QStringLiteral("清单");
        case IconId::Link: return QStringLiteral("链接");
        case IconId::Underline: return QStringLiteral("下划线");
        case IconId::Highlight: return QStringLiteral("高亮");
        case IconId::Expand: return QStringLiteral("展开");
        case IconId::Info: return QStringLiteral("信息");
        case IconId::ChevronDown: return QStringLiteral("展开选项");
        case IconId::FormatClear: return QStringLiteral("清除格式");
        case IconId::Format: return QStringLiteral("文本格式");
        case IconId::Comment: return QStringLiteral("活动记录");
        default: return QString();
    }
}

}  // namespace AppIcons
}  // namespace kl
