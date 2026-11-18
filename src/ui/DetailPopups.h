#ifndef KYLINLIST_DETAILPOPUPS_H
#define KYLINLIST_DETAILPOPUPS_H

#include <QFrame>
#include <QPointer>
#include "../domain/Entity.h"
#include "PopupMenu.h"

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
    void setDocument(const Entity& entity, const QString& title, const QString& plainBody);
private:
    QLabel *m_words = nullptr, *m_characters = nullptr;
    QLabel *m_created = nullptr, *m_updated = nullptr;
};

class ListDestinationMenu : public PopupMenu {
    Q_OBJECT
public:
    explicit ListDestinationMenu(QWidget* parent = nullptr);
    void setLists(const QVector<TaskList>& lists, const QVector<Folder>& folders,
                  const QString& selectedId);
    void popupAbove(QWidget* anchor);
signals:
    void listSelected(QString listId);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    QAction* addDestination(QMenu* menu, const TaskList& list, const QString& path, bool searchResult);
    void filterLists();
    QLabel* m_searchIcon = nullptr;
    QLineEdit* m_search = nullptr;
    QAction* m_empty = nullptr;
    QVector<QAction*> m_rootActions;
    QVector<QAction*> m_searchActions;
    QVector<QPointer<PopupMenu>> m_folderMenus;
    QString m_selectedId;
};

class ListSelectPopup : public ListDestinationMenu {
    Q_OBJECT
public:
    explicit ListSelectPopup(QWidget* parent = nullptr);
};

} // namespace kl
#endif // KYLINLIST_DETAILPOPUPS_H
