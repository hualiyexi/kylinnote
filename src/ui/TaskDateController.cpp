#include "TaskDateController.h"

#include <QWidget>
#include "../app/TaskStore.h"
#include "DatePickerPopup.h"

namespace kl {

TaskDateController::TaskDateController(TaskStore* store, QWidget* owner)
    : QObject(owner), m_store(store), m_owner(owner) {}

void TaskDateController::close() {
    if (m_popup) m_popup->reject();
}

void TaskDateController::open(const QString& entityId, const QRect& globalAnchor) {
    const Entity entity = m_store->entity(entityId);
    if (entity.id.isEmpty() || entity.kind != EntityKind::Task || entity.deleted) return;
    close();
    auto* picker = new DatePickerPopup(m_owner);
    m_popup = picker;
    if (entity.dueAt.isValid()) picker->setDateTime(entity.dueAt);
    else if (entity.allDayDate.isValid()) picker->setAllDayDate(entity.allDayDate);
    else picker->setDateTime(entity.startAt);
    picker->setReminder(entity.reminderAt);
    picker->setRecurrence(entity.recurrence);
    connect(picker, &QDialog::finished, this, [this, picker, entityId](int result) {
        const DatePickerPopup& state = *picker;
        if (result != QDialog::Accepted && !state.cleared()) return;
        // 弹层的实体身份固定；提交前读取最新状态，不能使用打开时的旧快照覆盖正文。
        Entity updated = m_store->entity(entityId);
        if (updated.id.isEmpty() || updated.deleted || updated.kind != EntityKind::Task) return;
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
        if (m_store->saveTask(updated)) emit saved(entityId);
        else emit saveFailed(QStringLiteral("日期保存失败，请重试。"));
    });
    connect(picker, &QDialog::finished, picker, &QObject::deleteLater);
    picker->popupAt(globalAnchor);
}

}  // namespace kl
