#include "TaskDateController.h"

#include <QWidget>
#include "../app/TaskStore.h"
#include "DatePickerPopup.h"

namespace kl {
namespace {

Entity withPickerState(const Entity& base, const DatePickerPopup& state) {
    Entity updated = base;
    if (!state.cleared() && state.hasDate()) {
        if (state.hasTime()) {
            updated.dueAt = state.dateTime();
            updated.allDayDate = QDate();
        } else {
            updated.dueAt = QDateTime();
            updated.allDayDate = state.allDayDate();
        }
        updated.startAt = QDateTime();
        updated.reminderAt = state.reminder();
        updated.recurrence = state.recurrence();
    } else {
        updated.dueAt = QDateTime();
        updated.startAt = QDateTime();
        updated.allDayDate = QDate();
        updated.reminderAt = QDateTime();
        updated.recurrence = QStringLiteral("none");
    }
    return updated;
}

}  // namespace

TaskDateController::TaskDateController(TaskStore* store, QWidget* owner)
    : QObject(owner), m_store(store), m_owner(owner) {}

bool TaskDateController::isOpen() const {
    return m_popup && m_popup->isVisible();
}

void TaskDateController::close() {
    if (m_popup) m_popup->reject();
}

void TaskDateController::open(const QString& entityId, const QRect& globalAnchor) {
    const Entity entity = m_store->entity(entityId);
    if (entity.id.isEmpty() || entity.kind != EntityKind::Task || entity.deleted) return;
    openEntity(entity, entityId, globalAnchor);
}

void TaskDateController::openDraft(const Entity& draft, const QRect& globalAnchor) {
    Entity entity = draft;
    entity.id.clear();
    entity.kind = EntityKind::Task;
    entity.deleted = false;
    openEntity(entity, QString(), globalAnchor);
}

void TaskDateController::openEntity(const Entity& entity, const QString& entityId,
                                    const QRect& globalAnchor) {
    close();
    auto* picker = new DatePickerPopup(m_owner);
    m_popup = picker;
    if (entity.dueAt.isValid()) picker->setDateTime(entity.dueAt);
    else if (entity.allDayDate.isValid()) picker->setAllDayDate(entity.allDayDate);
    else picker->setDateTime(entity.startAt);
    picker->setReminder(entity.reminderAt);
    picker->setRecurrence(entity.recurrence);
    connect(picker, &QDialog::finished, this, [this, picker, entity, entityId](int result) {
        const DatePickerPopup& state = *picker;
        if (result == QDialog::Accepted || state.cleared()) {
            // 已有任务提交前读取最新实体，不能用打开时的旧快照覆盖其他字段。
            Entity updated = entityId.isEmpty() ? entity : m_store->entity(entityId);
            if (entityId.isEmpty()
                || (!updated.id.isEmpty() && !updated.deleted && updated.kind == EntityKind::Task)) {
                updated = withPickerState(updated, state);
                if (entityId.isEmpty()) {
                    emit draftSaved(updated);
                } else if (m_store->saveTask(updated)) {
                    emit saved(entityId);
                } else {
                    emit saveFailed(QStringLiteral("日期保存失败，请重试。"));
                }
            }
        }
        emit closed();
    });
    connect(picker, &QDialog::finished, picker, &QObject::deleteLater);
    picker->popupAt(globalAnchor);
}

}  // namespace kl
