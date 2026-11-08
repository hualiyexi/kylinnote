#pragma once
#include <QDialog>
#include "../domain/Entity.h"
namespace kl {
class ReminderPopup : public QDialog {
    Q_OBJECT
public:
    explicit ReminderPopup(const Entity& task, QWidget* parent = nullptr);
    ReminderPopup(const Entity& task, QString reminderId, QWidget* parent = nullptr);
    QString entityId() const { return m_entityId; }
    QString reminderId() const { return m_reminderId; }
signals:
    void completeRequested(QString entityId);
    void snoozeRequested(QString entityId, QString reminderId, int minutes);
    void openRequested(QString entityId);
private:
    QString m_entityId;
    QString m_reminderId;
};
}  // namespace kl
