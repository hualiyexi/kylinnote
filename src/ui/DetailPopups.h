#ifndef KYLINLIST_DETAILPOPUPS_H
#define KYLINLIST_DETAILPOPUPS_H

#include <QFrame>
#include "../domain/Entity.h"

class QLabel;
class QLineEdit;
class QTreeWidget;

namespace kl {

class DetailPopup : public QFrame {
    Q_OBJECT
public:
    explicit DetailPopup(QWidget* parent = nullptr);
    void popupAt(QWidget* anchor, bool above, bool alignRight = false);
protected:
    QWidget* card() const { return m_card; }
private:
    void applyTheme();
    QWidget* m_card = nullptr;
};

class EntityInfoPopup : public DetailPopup {
    Q_OBJECT
public:
    explicit EntityInfoPopup(QWidget* parent = nullptr);
    void setDocument(const Entity& entity, const QString& title, const QString& plainBody,
                     const QStringList& checklist);
private:
    QLabel *m_words = nullptr, *m_characters = nullptr;
    QLabel *m_created = nullptr, *m_updated = nullptr;
};

class ListSelectPopup : public DetailPopup {
    Q_OBJECT
public:
    explicit ListSelectPopup(QWidget* parent = nullptr);
    void setLists(const QVector<TaskList>& lists, const QVector<Folder>& folders,
                  const QString& selectedId);
    void popupAbove(QWidget* anchor);
signals:
    void listSelected(QString listId);
private:
    void filterLists(const QString& query);
    QLineEdit* m_search = nullptr;
    QTreeWidget* m_tree = nullptr;
    QLabel* m_empty = nullptr;
};

} // namespace kl
#endif // KYLINLIST_DETAILPOPUPS_H
