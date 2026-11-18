#ifndef KYLINLIST_ENTITYPICKER_H
#define KYLINLIST_ENTITYPICKER_H

#include <QDialog>
#include <QPointer>
#include <QVector>
#include "../domain/Entity.h"
class QLineEdit;
class QListWidget;
class QDialogButtonBox;
namespace kl {
class TaskStore;
// Shared by the picker and inline completion; title search never changes identity.
QVector<Entity> matchingEntities(TaskStore* store, const QString& query);
class EntityPickerDialog : public QDialog {
    Q_OBJECT
public:
    explicit EntityPickerDialog(TaskStore* store, QWidget* parent = nullptr);
    Entity selectedEntity() const;
public slots:
    void accept() override;
private:
    void populate(const QString& search);
    QPointer<TaskStore> m_store;
    QLineEdit* m_search = nullptr;
    QListWidget* m_list = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
};
} // namespace kl

#endif // KYLINLIST_ENTITYPICKER_H
