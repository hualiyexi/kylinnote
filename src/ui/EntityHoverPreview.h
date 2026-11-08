#ifndef KYLINLIST_ENTITYHOVERPREVIEW_H
#define KYLINLIST_ENTITYHOVERPREVIEW_H

#include <QPointer>
#include <QTimer>
#include <QWidget>
class QLabel;
namespace kl {
class TaskStore;
class EntityHoverPreview : public QWidget {
    Q_OBJECT
public:
    explicit EntityHoverPreview(TaskStore* store, QWidget* parent = nullptr);
    void schedule(const QString& entityId, const QPoint& globalPosition);
    void showEntity(const QString& entityId, const QPoint& globalPosition);
    void cancel();
    void refresh();
private:
    QPointer<TaskStore> m_store;
    QTimer m_timer;
    QLabel *m_title = nullptr, *m_meta = nullptr, *m_summary = nullptr;
    QString m_pendingId, m_currentId;
    QPoint m_pendingPosition;
};
} // namespace kl

#endif // KYLINLIST_ENTITYHOVERPREVIEW_H
