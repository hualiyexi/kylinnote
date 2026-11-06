#pragma once
#include <QDialog>
#include "../domain/Task.h"
namespace kn {
class ReminderPopup : public QDialog {
    Q_OBJECT
public:
    explicit ReminderPopup(const Task& task, QWidget* parent = nullptr);
signals:
    void completeRequested(qint64 id);
    void snoozeRequested(qint64 id, int minutes);
    void openRequested(qint64 id);
};
}  // namespace kn
