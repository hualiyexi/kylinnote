#pragma once
#include <QDialog>
#include "../domain/Entity.h"

class QLabel;
class QPushButton;
class QToolButton;

namespace kl {
class PopupMenu;
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
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
private:
    void applyTheme();
    void positionOnScreen();
    void updateTitleElision();
    void positionSnoozeMenu(PopupMenu* menu);

    QString m_entityId;
    QString m_reminderId;
    QWidget* m_card = nullptr;
    QLabel* m_scheduleIcon = nullptr;
    QPushButton* m_title = nullptr;
    QToolButton* m_close = nullptr;
    QToolButton* m_snooze = nullptr;
};
}  // namespace kl
