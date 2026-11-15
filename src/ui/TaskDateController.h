#pragma once

#include <QObject>
#include <QPointer>
#include <QRect>
#include "../domain/Entity.h"

class QWidget;
namespace kl {
class DatePickerPopup;
class TaskStore;

// 列表行、详情与快速添加共用日期表单和状态转换；DatePickerPopup 只管理临时表单值。
class TaskDateController : public QObject {
    Q_OBJECT
public:
    TaskDateController(TaskStore* store, QWidget* owner);
    void open(const QString& entityId, const QRect& globalAnchor);
    void openDraft(const Entity& draft, const QRect& globalAnchor);
    bool isOpen() const;
    void close();

signals:
    void saved(QString entityId);
    void draftSaved(Entity draft);
    void saveFailed(QString message);
    void closed();

private:
    void openEntity(const Entity& entity, const QString& entityId, const QRect& globalAnchor);

    TaskStore* m_store;
    QWidget* m_owner;
    QPointer<DatePickerPopup> m_popup;
};
}  // namespace kl
