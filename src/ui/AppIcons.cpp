#include "AppIcons.h"

#include <QByteArray>
#include <QCache>
#include <QPainter>
#include <QSvgRenderer>
#include <cstring>
#include "../Common.h"
#include "design/ThemeManager.h"
#include "ui/icons/IconData.gen.h"

namespace kl {
namespace AppIcons {
namespace {

// 语义枚举 -> 图标名。换图标只动这张表。
QString iconNameFor(IconId id) {
    switch (id) {
        case IconId::All: return "all-list";
        case IconId::Inbox: return "inbox-list";
        case IconId::Today:
            return QStringLiteral("today-icon-%1").arg(toAppTime(appNow()).date().day());
        case IconId::Tomorrow: return "tomorrow-list";
        case IconId::Upcoming: return "today-icon-su";
        case IconId::Overdue: return "custom-smart-list";
        case IconId::Done: return "sidebar-completed-list";
        case IconId::Abandoned: return "wont-do";
        case IconId::Trash: return "delete";
        case IconId::Folder: return "folder";
        case IconId::Note: return "note";
        case IconId::TaskList: return "normal-list";
        case IconId::Pin: return "pin-task";
        case IconId::Unpin: return "unpin-task";
        case IconId::AddSubtask: return "subtask-bold";
        case IconId::LinkParent: return "bind-parent";
        case IconId::MoveTo: return "move-list";
        case IconId::Copy: return "copy";
        case IconId::PinSection: return "pin-task-section";
        case IconId::Archive: return "archive";
        case IconId::Unarchive: return "unarchive";
        case IconId::ArchivedLists: return "closed-list";
        case IconId::SmartSettings: return "setting-smart";

        case IconId::Search: return "search";
        case IconId::More: return "more";
        case IconId::Add: return "add";
        case IconId::Close: return "close";
        case IconId::Check: return "selected";
        case IconId::Calendar: return "calendar-schedule";
        case IconId::Clock: return "timecard_time";
        case IconId::Reminder: return "item-reminder";
        case IconId::Repeat: return "item-repeat";
        case IconId::FlagHigh: return "priority-3";
        case IconId::FlagMedium: return "priority-2";
        case IconId::FlagLow: return "priority-1";
        case IconId::FlagNone: return "priority-0";
        case IconId::Attachment: return "attachment";
        case IconId::Tag: return "tag";
        case IconId::ActivityLog: return "timeline";
        case IconId::Convert: return "convert-task";
        case IconId::Restore: return "restore";
        case IconId::Delete: return "delete";
        case IconId::DesktopNote: return "mobile-edit";
        case IconId::Settings: return "setting";
        case IconId::Backup: return "s-backup";
        case IconId::Theme: return "light";

        case IconId::Bold: return "md-tl-bold";
        case IconId::Italic: return "md-tl-italic";
        case IconId::Strike: return "md-tl-strikethrough";
        case IconId::Heading: return "md-tl-heading";
        case IconId::List: return "md-tl-list-1";
        case IconId::OrderedList: return "md-tl-list-2";
        case IconId::Quote: return "md-tl-quote";
        case IconId::Code: return "md-tl-code-inline";
        case IconId::Checklist: return "md-item-checked";
        case IconId::Link: return "md-tl-link";
        case IconId::Underline: return "md-tl-underline";
        case IconId::Highlight: return "md-tl-mark";
        case IconId::Expand: return "md-tl-fullscreen";
        case IconId::Info: return "note-info";
        case IconId::ChevronDown: return "thin-triangle-down";
        case IconId::FormatClear: return "clear-input";
        case IconId::Format: return "md-tl-text";
        case IconId::Comment: return "comment";

        case IconId::WindowMinimize: return "minimized";
        case IconId::WindowMaximize: return "md-tl-fullscreen";
        case IconId::WindowClose: return "bold_close";
        case IconId::Sort: return "sort";
        case IconId::MoreTitlebar: return "more-titlebar";
        case IconId::SidebarCollapse: return "sidebar-collapse";
        case IconId::SidebarExpand: return "sidebar-expand";
        case IconId::HideCompleted: return "hide-completed";
        case IconId::HideDetails: return "hide-details";
        case IconId::DisplaySettings: return "calendar-config";
        case IconId::Help: return "help";
        case IconId::Sync: return "sidebar-sync";

        case IconId::None:
        case IconId::Count:
            break;
    }
    return QString();
}

// 把一条图标记录拼成完整 SVG 文档。单色图标注入主题色，多色图标保留原始根属性。
QByteArray buildSvg(const icons::IconEntry& entry, const QColor& color) {
    QByteArray svg;
    svg.reserve(static_cast<int>(std::strlen(entry.body)) + 160);
    svg.append("<svg xmlns='http://www.w3.org/2000/svg' viewBox='");
    svg.append(entry.viewBox);
    svg.append("'");
    if (entry.mono) {
        svg.append(" fill='");
        svg.append(color.name().toLatin1());
        svg.append("'");
        if (color.alpha() < 255) {
            // 半透明由 fill-opacity 承担，Qt 5.12 的 SVG 解析不认 rgba() 写法。
            svg.append(" fill-opacity='");
            svg.append(QByteArray::number(color.alphaF(), 'f', 3));
            svg.append("'");
        }
    } else if (entry.rootAttrs && entry.rootAttrs[0] != '\0') {
        svg.append(' ');
        svg.append(entry.rootAttrs);
    }
    svg.append('>');
    svg.append(entry.body);
    svg.append("</svg>");
    return svg;
}

// 解析过的 SVG 缓存：delegate 每帧每行都取图标，重复解析 2.5 KB 路径会拖慢绘制。
QSvgRenderer* renderer(const icons::IconEntry& entry, const QColor& color) {
    static QCache<QString, QSvgRenderer> cache(256);
    const QString key = QString::fromLatin1(entry.id) + QLatin1Char('|') +
                        color.name(QColor::HexArgb);
    if (QSvgRenderer* cached = cache.object(key)) return cached;
    auto* svg = new QSvgRenderer(buildSvg(entry, color));
    if (!svg->isValid()) {
        delete svg;
        return nullptr;
    }
    cache.insert(key, svg, 1);
    return svg;
}

// 按 viewBox 宽高比居中放进目标矩形；非方形图标不会被拉扁。
QRectF fitRect(const QRectF& target, const QRectF& viewBox) {
    if (viewBox.width() <= 0.0 || viewBox.height() <= 0.0) return target;
    const qreal scale =
        qMin(target.width() / viewBox.width(), target.height() / viewBox.height());
    if (scale <= 0.0) return target;
    const QSizeF size(viewBox.width() * scale, viewBox.height() * scale);
    return QRectF(target.center() - QPointF(size.width() / 2.0, size.height() / 2.0), size);
}

QPixmap renderPixmap(const icons::IconEntry& entry, const QColor& color, int size,
                     qreal dpr) {
    QPixmap pixmap(qRound(size * dpr), qRound(size * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QSvgRenderer* svg = renderer(entry, color);
    if (!svg) return pixmap;
    QPainter painter(&pixmap);
    svg->render(&painter, fitRect(QRectF(0, 0, size, size), svg->viewBoxF()));
    return pixmap;
}

QIcon buildIcon(const icons::IconEntry* entry, const QColor& color, int size) {
    if (!entry) return QIcon();
    constexpr qreal dpr = 2.0;  // 高 DPI 超采样
    QIcon result;
    result.addPixmap(renderPixmap(*entry, color, size, dpr));
    return result;
}

void paintEntry(QPainter* painter, const icons::IconEntry* entry, const QRectF& rect,
                const QColor& color) {
    if (!painter || !entry) return;
    QSvgRenderer* svg = renderer(*entry, color);
    if (!svg) return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    svg->render(painter, fitRect(rect, svg->viewBoxF()));
    painter->restore();
}

const icons::IconEntry* lookup(IconId id) {
    const QString name = iconNameFor(id);
    return name.isEmpty() ? nullptr : icons::find(name.toUtf8().constData());
}

const icons::IconEntry* lookup(const QString& name) {
    if (name.isEmpty()) return nullptr;
    return icons::find(name.toUtf8().constData());
}

}  // namespace

QIcon icon(IconId id, int size) {
    return icon(id, ds::ColorToken::TextSecondary, size);
}

QIcon icon(IconId id, ds::ColorToken colorToken, int size) {
    return buildIcon(lookup(id), ThemeManager::instance().color(colorToken), size);
}

void paint(QPainter* painter, IconId id, const QRectF& rect, const QColor& color) {
    paintEntry(painter, lookup(id), rect, color);
}

QIcon icon(const QString& name, int size) {
    return icon(name, ds::ColorToken::TextSecondary, size);
}

QIcon icon(const QString& name, ds::ColorToken colorToken, int size) {
    return buildIcon(lookup(name), ThemeManager::instance().color(colorToken), size);
}

QIcon icon(const QString& name, const QColor& color, int size) {
    return buildIcon(lookup(name), color, size);
}

void paint(QPainter* painter, const QString& name, const QRectF& rect, const QColor& color) {
    paintEntry(painter, lookup(name), rect, color);
}

bool hasIcon(const QString& name) {
    return lookup(name) != nullptr;
}

QString iconName(IconId id) {
    return iconNameFor(id);
}

QString accessibleName(IconId id) {
    switch (id) {
        case IconId::All: return QStringLiteral("所有");
        case IconId::Inbox: return QStringLiteral("收集箱");
        case IconId::Today: return QStringLiteral("今天");
        case IconId::Tomorrow: return QStringLiteral("明天");
        case IconId::Upcoming: return QStringLiteral("最近7天");
        case IconId::Overdue: return QStringLiteral("已逾期");
        case IconId::Done: return QStringLiteral("已完成");
        case IconId::Abandoned: return QStringLiteral("已放弃");
        case IconId::Trash: return QStringLiteral("回收站");
        case IconId::Folder: return QStringLiteral("文件夹");
        case IconId::Note: return QStringLiteral("笔记");
        case IconId::TaskList: return QStringLiteral("任务清单");
        case IconId::Pin: return QStringLiteral("置顶");
        case IconId::Unpin: return QStringLiteral("取消置顶");
        case IconId::AddSubtask: return QStringLiteral("添加子任务");
        case IconId::LinkParent: return QStringLiteral("关联主任务");
        case IconId::MoveTo: return QStringLiteral("移动到");
        case IconId::Copy: return QStringLiteral("创建副本");
        case IconId::PinSection: return QStringLiteral("已置顶分区");
        case IconId::Archive: return QStringLiteral("归档清单");
        case IconId::Unarchive: return QStringLiteral("取消归档");
        case IconId::ArchivedLists: return QStringLiteral("已归档的清单");
        case IconId::SmartSettings: return QStringLiteral("智能清单设置");
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
        case IconId::WindowMinimize: return QStringLiteral("最小化");
        case IconId::WindowMaximize: return QStringLiteral("最大化");
        case IconId::WindowClose: return QStringLiteral("关闭窗口");
        case IconId::Sort: return QStringLiteral("排序与视图");
        case IconId::MoreTitlebar: return QStringLiteral("视图选项");
        case IconId::SidebarCollapse: return QStringLiteral("隐藏筛选栏");
        case IconId::SidebarExpand: return QStringLiteral("显示筛选栏");
        case IconId::HideCompleted: return QStringLiteral("显示或隐藏已完成");
        case IconId::HideDetails: return QStringLiteral("显示或隐藏详细");
        case IconId::DisplaySettings: return QStringLiteral("显示设置");
        case IconId::Help: return QStringLiteral("帮助");
        case IconId::Sync: return QStringLiteral("本机数据");
        default: return QString();
    }
}

}  // namespace AppIcons
}  // namespace kl
