#include "TaskDateController.h"

#include <QWidget>
#include "../app/TaskStore.h"
#include "DatePickerPopup.h"

namespace kl {

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
    if (entity.id.isEmpty() || entity.deleted) return;
    openEntity(entity, entityId, globalAnchor, m_store->schedule(entityId));
}

void TaskDateController::openDraft(const Entity& draft, const QRect& globalAnchor) {
    openDraft(draft, scheduleFromEntity(draft), globalAnchor);
}

void TaskDateController::openDraft(const Entity& draft, const ScheduleValue& schedule,
                                   const QRect& globalAnchor) {
    Entity entity = draft;
    entity.id.clear();
    entity.deleted = false;
    openEntity(entity, QString(), globalAnchor, schedule);
}

void TaskDateController::openEntity(const Entity& entity, const QString& entityId,
                                    const QRect& globalAnchor, const ScheduleValue& initial) {
    close();
    auto* picker = new DatePickerPopup(m_owner);
    m_popup = picker;
    picker->setRecurrenceEnabled(entity.kind == EntityKind::Task);
    picker->setSchedule(initial);
    const ScheduleValue baseline = initial;
    connect(picker, &QDialog::finished, this,
            [this, picker, entity, entityId, baseline](int result) {
        const DatePickerPopup& state = *picker;
        if (result != QDialog::Accepted && !state.cleared()) {
            emit closed();
            return;
        }
        ScheduleValue schedule = state.cleared() ? ScheduleValue() : state.schedule();
        if (state.cleared()) {
            schedule.timeZone = baseline.timeZone;
        }
        if (entityId.isEmpty()) {
            emit draftSaved(entity, schedule);
            emit closed();
            return;
        }
        // 日期、提醒和重复基线独立比较；标题等无关修改正常合并。
        const Entity latest = m_store->entity(entityId);
        if (latest.id.isEmpty() || latest.deleted) {
            emit saveFailed(QStringLiteral("实体已删除，日期草稿未写入。"));
        } else if (!sameScheduleBaseline(m_store->schedule(entityId), baseline)) {
            emit saveFailed(QStringLiteral("日期或提醒已在其他窗口修改，请重新打开日期选择器。"));
        } else if (m_store->saveSchedule(latest, schedule)) {
            emit saved(entityId);
        } else {
            emit saveFailed(QStringLiteral("日期保存失败：") + m_store->lastError());
        }
        emit closed();
    });
    connect(picker, &QDialog::finished, picker, &QObject::deleteLater);
    picker->popupAt(globalAnchor);
}

}  // namespace kl
