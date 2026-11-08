#ifndef KYLINLIST_ENTITYPICKER_H
#define KYLINLIST_ENTITYPICKER_H

#include <QDialog>
#include <QPointer>
#include "../domain/Entity.h"
class QLineEdit;
class QListWidget;
class QDialogButtonBox;
namespace kl {
class TaskStore;
class EntityPickerDialog : public QDialog {
    Q_OBJECT
public:
    explicit EntityPickerDialog(TaskStore* store, QWidget* parent = nullptr);
    Entity selectedEntity() const;
private:
    void populate(const QString& search);
    QPointer<TaskStore> m_store;
    QLineEdit* m_search = nullptr;
    QListWidget* m_list = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    QVector<Entity> m_entities;
};
} // namespace kl

#endif // KYLINLIST_ENTITYPICKER_H
