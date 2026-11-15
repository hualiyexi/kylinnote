#pragma once
// 集中式 SVG 图标工厂：图标几何来自内置图标集（src/ui/icons/IconData.gen.cpp），
// 单色图标随主题着色，统一高 DPI 与可访问名称。
//
// 两条取用路径：
//   1) icon(IconId)       —— 语义枚举，界面代码用它，换图标只改映射表；
//   2) icon("quick-today") —— 直接按图标名取用，949 个图标都可点名。
#include <QIcon>
#include <QPixmap>
#include <QString>
#include "design/DesignTokens.h"

class QPainter;
namespace kl {

enum class IconId {
    None,
    // 导航
    All, Inbox, Today, Tomorrow, Upcoming, Overdue, Done, Abandoned, Trash,
    Folder, Note, TaskList, Pin, PinSection, Archive, Unarchive, ArchivedLists, SmartSettings,
    // 操作
    Search, More, Add, Close, Check,
    Calendar, Clock, Reminder, Repeat,
    FlagHigh, FlagMedium, FlagLow, FlagNone,
    Attachment, Tag, ActivityLog, Convert, Restore, Delete,
    DesktopNote, Settings, Backup, Theme,
    // 编辑器
    Bold, Italic, Strike, Heading, List, OrderedList, Quote, Code, Checklist, Link,
    Underline, Highlight, Expand, Info, ChevronDown, FormatClear, Format, Comment,
    // 主窗口
    WindowMinimize, WindowMaximize, WindowClose, Sort, Help, Sync,
    Count
};

namespace AppIcons {
// 返回按当前主题着色的图标。size 为逻辑像素边长。
QIcon icon(IconId id, int size = 16);
// 用指定语义色渲染（用于选中态等）。
QIcon icon(IconId id, ds::ColorToken colorToken, int size = 16);
// 直接绘制到 painter（供 delegate 自绘使用）。
void paint(QPainter* painter, IconId id, const QRectF& rect, const QColor& color);

// 按图标名直取（如 "quick-today"）。名字不存在时返回空图标。
QIcon icon(const QString& name, int size = 16);
QIcon icon(const QString& name, ds::ColorToken colorToken, int size = 16);
QIcon icon(const QString& name, const QColor& color, int size = 16);
void paint(QPainter* painter, const QString& name, const QRectF& rect, const QColor& color);
bool hasIcon(const QString& name);
// 枚举对应的图标名；没有对应时返回空串。
QString iconName(IconId id);

// 图标的可访问中文名称。
QString accessibleName(IconId id);
}  // namespace AppIcons

}  // namespace kl
