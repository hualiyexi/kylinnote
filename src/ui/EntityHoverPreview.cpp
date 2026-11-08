#include "EntityHoverPreview.h"

#include <QGuiApplication>
#include <QLabel>
#include <QScreen>
#include <QVBoxLayout>
#include "../app/TaskStore.h"
#include "../domain/ViewSpec.h"
#include "AppStyle.h"
#include "Markdown.h"
#include "design/ThemeManager.h"

namespace kl {
EntityHoverPreview::EntityHoverPreview(TaskStore* store, QWidget* parent)
    : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint), m_store(store), m_timer(this) {
    setObjectName(QStringLiteral("EntityHoverPreview")); setAttribute(Qt::WA_DeleteOnClose, false);
    setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(14, 10, 14, 10); layout->setSpacing(5);
    m_title = new QLabel(this); m_title->setObjectName(QStringLiteral("HoverTitle")); m_title->setFont(AppStyle::font(AppStyle::Font::Body));
    m_meta = new QLabel(this); m_meta->setObjectName(QStringLiteral("HoverMeta")); m_meta->setFont(AppStyle::font(AppStyle::Font::Small));
    m_summary = new QLabel(this); m_summary->setObjectName(QStringLiteral("HoverSummary")); m_summary->setWordWrap(true);
    m_summary->setMaximumWidth(360); m_summary->setFont(AppStyle::font(AppStyle::Font::Small));
    layout->addWidget(m_title); layout->addWidget(m_meta); layout->addWidget(m_summary);
    m_timer.setObjectName(QStringLiteral("HoverDelay")); m_timer.setSingleShot(true); m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, [this]() {
        const QString id = m_pendingId; const QPoint position = m_pendingPosition;
        m_pendingId.clear(); if (!id.isEmpty()) showEntity(id, position);
    });
    if (m_store) {
        connect(m_store, &TaskStore::taskChanged, this, [this](QString) { refresh(); });
        connect(m_store, &TaskStore::tasksReset, this, &EntityHoverPreview::refresh);
    }
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() { applyTheme(); });
    hide();
}
void EntityHoverPreview::applyTheme() {
    const ThemeManager& theme = ThemeManager::instance();
    setStyleSheet(QStringLiteral(
                      "#EntityHoverPreview{background:%1;border:1px solid %2;}"
                      "#EntityHoverPreview QLabel{background:transparent;border:0;}"
                      "#HoverTitle,#HoverSummary{color:%3;}"
                      "#HoverMeta{color:%4;}")
                      .arg(theme.color(ds::ColorToken::TooltipBg).name())
                      .arg(theme.color(ds::ColorToken::Border).name())
                      .arg(theme.color(ds::ColorToken::TooltipText).name())
                      .arg(theme.color(ds::ColorToken::TextSecondary).name()));
}
void EntityHoverPreview::schedule(const QString& entityId, const QPoint& globalPosition) {
    if (!m_store || m_store->entity(entityId).id.isEmpty()) { cancel(); return; }
    if (entityId != m_currentId) { m_currentId.clear(); hide(); }
    m_pendingId = entityId; m_pendingPosition = globalPosition; m_timer.start();
}
void EntityHoverPreview::showEntity(const QString& entityId, const QPoint& globalPosition) {
    m_timer.stop(); m_pendingId.clear();
    if (!m_store) { hide(); return; }
    const Entity entity = m_store->entity(entityId);
    if (entity.id.isEmpty()) { cancel(); return; }
    m_currentId = entityId;
    m_title->setText(entity.title);
    QString status;
    if (entity.kind == EntityKind::Note) status = QStringLiteral("笔记");
    else if (entity.deleted) status = QStringLiteral("任务 · 回收站");
    else if (entity.status == TaskStatus::Completed) status = QStringLiteral("任务 · 已完成");
    else if (entity.status == TaskStatus::Abandoned) status = QStringLiteral("任务 · 已放弃");
    else {
        const QDate date = effectiveDate(entity);
        status = date.isValid() && date < toAppTime(appNow()).date() ? QStringLiteral("任务 · 已过期")
                                                                    : QStringLiteral("任务 · 未完成");
    }
    m_meta->setText(status);
    QString summary = Markdown::plainText(entity, Markdown::entityMap(m_store->entities())).simplified();
    if (summary.size() > 220) summary = summary.left(219) + QChar(0x2026);
    m_summary->setText(summary.isEmpty() ? QStringLiteral("（无正文）") : summary);
    adjustSize();
    QPoint position = globalPosition + QPoint(12, 16);
    if (QScreen* screen = QGuiApplication::screenAt(globalPosition)) {
        const QRect available = screen->availableGeometry();
        if (position.x() + width() > available.right()) position.setX(available.right() - width());
        if (position.y() + height() > available.bottom()) position.setY(globalPosition.y() - height() - 12);
    }
    move(position); show(); raise();
}
void EntityHoverPreview::cancel() {
    m_timer.stop(); m_pendingId.clear(); m_currentId.clear(); hide();
}
void EntityHoverPreview::refresh() {
    if (!m_pendingId.isEmpty() && (!m_store || m_store->entity(m_pendingId).id.isEmpty())) { cancel(); return; }
    if (!m_currentId.isEmpty()) {
        if (!m_store || m_store->entity(m_currentId).id.isEmpty()) { cancel(); return; }
        if (isVisible()) showEntity(m_currentId, m_pendingPosition.isNull() ? mapToGlobal(pos()) : m_pendingPosition);
    }
}
} // namespace kl
