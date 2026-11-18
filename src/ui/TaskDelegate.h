#pragma once
#include <QColor>
#include <QStyledItemDelegate>
#include <QStringList>
#include "AppIcons.h"
#include "AppStyle.h"
#include "design/DesignTokens.h"
namespace kl {

// 保留旧枚举名作为兼容别名，新代码用 IconId。
using NavIcon = IconId;
inline void drawNavIcon(QPainter* painter, IconId icon, const QRectF& rect, const QColor& color) {
    AppIcons::paint(painter, icon, rect, color);
}

// 任务行：圆角 checkbox + 标题 + 元信息，选中为柔和蓝。
class TaskDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit TaskDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {
    }
    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    // 与普通行共用布局；仅拖动预览省略清单色条，避免它变成独立的悬浮色块。
    void paintDragPreview(QPainter* painter,
                          const QStyleOptionViewItem& option,
                          const QModelIndex& index) const;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    bool helpEvent(QHelpEvent* event, QAbstractItemView* view,
                   const QStyleOptionViewItem& option, const QModelIndex& index) override;
    bool editorEvent(QEvent* event,
                     QAbstractItemModel* model,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override;
    // 行内日期点击区域；与 editorEvent 共用同一布局，供视图设置手型指针。
    QRect dateHitRect(const QStyleOptionViewItem& option, const QModelIndex& index) const;
    // 按实例主题覆盖，默认关闭：开启后本 delegate 的全部语义色（含 AppIcons
    // 图标着色）改从指定主题取值；未开启时仍走全局 ThemeManager，行为与原先完全一致。
    void setThemeOverride(ds::Theme theme);
    void clearThemeOverride();
    bool hasThemeOverride() const;
signals:
    void dateActivated(QString entityId, QRect anchor);
    void listActivated(QString listId);
    void postponeActivated(QStringList entityIds);
    void childrenToggleActivated(QString entityId);
    void sectionToggleActivated(QString sectionKey);
private:
    void paintInternal(QPainter* painter,
                       const QStyleOptionViewItem& option,
                       const QModelIndex& index,
                       bool dragPreview) const;
    QColor tokenColor(ds::ColorToken token) const;
    QColor styleColor(AppStyle::Color role) const;
    bool m_themeOverrideActive = false;
    ds::Theme m_themeOverride = ds::Theme::Light;
};

// 侧栏导航行：分区标题、SVG 图标、独立右对齐数量。
class NavigationDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    // 与 TaskPanel 现有 UserRole(view)/+1(group)/+2(label) 并存。
    enum Role {
        CountRole = Qt::UserRole + 3,
        SectionRole = Qt::UserRole + 4,
        IconRole = Qt::UserRole + 5,
        FolderIdRole = Qt::UserRole + 6,
        IconNameRole = Qt::UserRole + 7,
        ListColorRole = Qt::UserRole + 8,
        IndentRole = Qt::UserRole + 9,
        ListKindRole = Qt::UserRole + 10,
        DividerRole = Qt::UserRole + 11,
        ParentFolderRole = Qt::UserRole + 12,
        DropTargetRole = Qt::UserRole + 13,
        DropLineRole = Qt::UserRole + 14,
        FolderExpandedRole = Qt::UserRole + 15,
        OrganizationEndRole = Qt::UserRole + 16,
        SmartRowRole = Qt::UserRole + 17
    };
    explicit NavigationDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {
    }
    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    bool helpEvent(QHelpEvent* event, QAbstractItemView* view,
                   const QStyleOptionViewItem& option, const QModelIndex& index) override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option, const QModelIndex& index) override;
    // 同 TaskDelegate：按实例主题覆盖，默认关闭。
    void setThemeOverride(ds::Theme theme);
    void clearThemeOverride();
    bool hasThemeOverride() const;
signals:
    void folderToggleActivated(QString folderId);
private:
    QColor tokenColor(ds::ColorToken token) const;
    QColor styleColor(AppStyle::Color role) const;
    bool m_themeOverrideActive = false;
    ds::Theme m_themeOverride = ds::Theme::Light;
};
}  // namespace kl
