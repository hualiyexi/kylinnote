#pragma once
// 集中式 SVG 图标工厂：单色 SVG 随主题着色，统一 accessibleName / toolTip / 高 DPI。
//
// 所有图标经 icon(IconId) 获取，按当前主题语义色渲染；深色主题下自动可见。
#include <QIcon>
#include <QPixmap>
#include <QString>
#include "design/DesignTokens.h"

class QPainter;
namespace kl {

enum class IconId {
    None,
    // 导航
    Inbox, Today, Upcoming, Overdue, Done, Abandoned, Trash,
    Folder, Note, TaskList, Pin,
    // 操作
    Search, More, Add, Close, Check,
    Calendar, Clock, Reminder, Repeat,
    FlagHigh, FlagMedium, FlagLow, FlagNone,
    Attachment, Tag, ActivityLog, Convert, Restore, Delete,
    DesktopNote, Settings, Backup, Theme,
    // 编辑器
    Bold, Italic, Strike, Heading, List, OrderedList, Quote, Code, Checklist, Link,
    Count
};

namespace AppIcons {
// 返回按当前主题着色的图标。size 为逻辑像素边长。
QIcon icon(IconId id, int size = 16);
// 用指定语义色渲染（用于选中态等）。
QIcon icon(IconId id, ds::ColorToken colorToken, int size = 16);
// 直接绘制到 painter（供 delegate 自绘使用）。
void paint(QPainter* painter, IconId id, const QRectF& rect, const QColor& color);
// 图标的可访问中文名称。
QString accessibleName(IconId id);
}  // namespace AppIcons

}  // namespace kl
