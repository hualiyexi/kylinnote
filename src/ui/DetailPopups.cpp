#include "DetailPopups.h"
#include "ListAppearance.h"

#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QScreen>
#include <QTextBoundaryFinder>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <algorithm>
#include "AppIcons.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {
namespace {
constexpr int shadowMargin = 10;

int wordCount(const QString& text) {
    QTextBoundaryFinder finder(QTextBoundaryFinder::Word, text);
    int count = 0;
    int start = 0;
    while (true) {
        const int end = finder.toNextBoundary();
        if (end < 0) break;
        const QString part = text.mid(start, end - start);
        for (const QChar ch : part) {
            if (ch.isLetterOrNumber()) {
                ++count;
                break;
            }
        }
        start = end;
    }
    return count;
}

QString displayDate(const QDateTime& date) {
    return date.isValid() ? toAppTime(date).toString(QStringLiteral("yyyy年M月d日"))
                          : QStringLiteral("—");
}
} // namespace

DetailPopup::DetailPopup(QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setFrameShape(QFrame::NoFrame);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(shadowMargin, shadowMargin, shadowMargin, shadowMargin);
    m_card = new QWidget(this);
    m_card->setObjectName(QStringLiteral("DetailPopupCard"));
    m_card->setAttribute(Qt::WA_StyledBackground);
    auto* shadow = new QGraphicsDropShadowEffect(m_card);
    shadow->setBlurRadius(16);
    shadow->setOffset(0, 3);
    m_card->setGraphicsEffect(shadow);
    outer->addWidget(m_card);
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this]() { applyTheme(); });
}

void DetailPopup::applyTheme() {
    const auto& theme = ThemeManager::instance();
    m_card->setStyleSheet(QStringLiteral(
        "QWidget#DetailPopupCard{background:%1;border:1px solid %2;border-radius:10px;}"
        "QLabel{background:transparent;border:0;}"
        "QLineEdit#ListSearch{background:transparent;border:0;border-radius:0;"
        "border-bottom:1px solid %2;padding:6px 4px;}"
        "QTreeWidget#ListSelectTree{background:transparent;border:0;outline:0;}"
        "QTreeWidget#ListSelectTree::item{height:32px;border:0;border-radius:5px;padding:0 4px;}"
        "QTreeWidget#ListSelectTree::item:hover{background:%3;}"
        "QTreeWidget#ListSelectTree::item:selected{background:%4;color:%5;}")
        .arg(theme.color(ds::ColorToken::MenuBg).name())
        .arg(theme.color(ds::ColorToken::Divider).name())
        .arg(theme.color(ds::ColorToken::MenuHover).name())
        .arg(theme.color(ds::ColorToken::Selection).name())
        .arg(theme.color(ds::ColorToken::Text).name()));
    QColor tint = theme.color(ds::ColorToken::Shadow);
    tint.setAlpha(36);
    static_cast<QGraphicsDropShadowEffect*>(m_card->graphicsEffect())->setColor(tint);
}

void DetailPopup::popupAt(QWidget* anchor, bool above, bool alignRight) {
    if (!anchor) return;
    layout()->activate();
    adjustSize();
    const QPoint top = anchor->mapToGlobal(QPoint(0, 0));
    const QPoint bottom = anchor->mapToGlobal(QPoint(0, anchor->height()));
    QPoint position(alignRight ? top.x() + anchor->width() - width() + shadowMargin
                              : top.x() - shadowMargin,
                    above ? top.y() - height() + shadowMargin - 4
                          : bottom.y() - shadowMargin + 4);
    QScreen* screen = QGuiApplication::screenAt(top);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect available = screen->availableGeometry();
        if (above && position.y() < available.top()) position.setY(bottom.y() + 4);
        if (!above && position.y() + height() > available.bottom()) position.setY(top.y() - height() - 4);
        position.setX(qBound(available.left(), position.x(), qMax(available.left(), available.right() - width() + 1)));
        position.setY(qBound(available.top(), position.y(), qMax(available.top(), available.bottom() - height() + 1)));
    }
    move(position);
    show();
    raise();
}

EntityInfoPopup::EntityInfoPopup(QWidget* parent) : DetailPopup(parent) {
    setObjectName(QStringLiteral("EntityInfoPopup"));
    setWindowTitle(QStringLiteral("信息"));
    card()->setMinimumWidth(124);
    auto* rows = new QVBoxLayout(card());
    rows->setContentsMargins(16, 10, 16, 12);
    rows->setSpacing(3);
    auto field = [this, rows](const QString& label, const QString& name, QLabel*& value) {
        if (rows->count()) rows->addSpacing(6);
        auto* heading = new QLabel(label, card());
        heading->setProperty("muted", true);
        heading->setFont(ds::qfont(ds::FontScene::Caption));
        rows->addWidget(heading);
        value = new QLabel(card());
        value->setObjectName(name);
        value->setFont(ds::qfont(ds::FontScene::Caption));
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        rows->addWidget(value);
    };
    field(QStringLiteral("字数"), QStringLiteral("DetailWordCount"), m_words);
    field(QStringLiteral("字符数"), QStringLiteral("DetailCharacterCount"), m_characters);
    field(QStringLiteral("创建于"), QStringLiteral("DetailCreatedAt"), m_created);
    field(QStringLiteral("修改于"), QStringLiteral("DetailUpdatedAt"), m_updated);
}

void EntityInfoPopup::setDocument(const Entity& entity, const QString& title,
                                  const QString& plainBody) {
    const QString text = title + QLatin1Char('\n') + plainBody;
    QString characters = text;
    characters.remove(QLatin1Char('\r'));
    characters.remove(QLatin1Char('\n'));
    m_words->setNum(wordCount(text));
    m_words->setToolTip(QStringLiteral("标题与正文中的中文字符及英文单词，不计格式标记与标点"));
    m_characters->setNum(characters.toUcs4().size());
    m_characters->setToolTip(QStringLiteral("标题与正文中的字符，含空格和标点，不含换行与格式标记"));
    m_created->setText(displayDate(entity.createdAt));
    m_updated->setText(displayDate(entity.updatedAt));
    m_created->setToolTip(toAppTime(entity.createdAt).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    m_updated->setToolTip(toAppTime(entity.updatedAt).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
}

ListDestinationMenu::ListDestinationMenu(QWidget* parent) : PopupMenu(parent) {
    setObjectName(QStringLiteral("ListDestinationMenu"));
    setProperty("entityMenu", true);
    setMinimumWidth(264);
    auto* host = new QWidget(this);
    host->setObjectName(QStringLiteral("ListSearchRow"));
    host->setFixedHeight(34);
    auto* row = new QHBoxLayout(host);
    row->setContentsMargins(12, 0, 12, 0);
    row->setSpacing(8);
    m_searchIcon = new QLabel(host);
    m_searchIcon->setObjectName(QStringLiteral("ListSearchIcon"));
    m_searchIcon->setFixedSize(18, 18);
    m_searchIcon->setAlignment(Qt::AlignCenter);
    const auto updateSearchIcon = [this]() {
        m_searchIcon->setPixmap(AppIcons::icon(QStringLiteral("search-thin"),
            ds::ColorToken::TextSecondary, 18).pixmap(18, 18));
    };
    updateSearchIcon();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [updateSearchIcon](ds::Theme) { updateSearchIcon(); });
    m_search = new QLineEdit(host);
    m_search->setObjectName(QStringLiteral("ListSearch"));
    m_search->setFixedHeight(18);
    m_search->setPlaceholderText(QStringLiteral("搜索清单"));
    m_search->setClearButtonEnabled(true);
    m_search->setAccessibleName(QStringLiteral("搜索清单"));
    m_search->installEventFilter(this);
    row->addWidget(m_searchIcon, 0, Qt::AlignVCenter);
    row->addWidget(m_search, 1, Qt::AlignVCenter);
    auto* searchAction = new QWidgetAction(this);
    searchAction->setObjectName(QStringLiteral("listSearchAction"));
    searchAction->setDefaultWidget(host);
    addAction(searchAction);
    addSeparator();
    m_empty = new QAction(QStringLiteral("没有匹配清单"), this);
    m_empty->setObjectName(QStringLiteral("listDestinationEmpty"));
    m_empty->setEnabled(false);
    m_empty->setVisible(false);
    addAction(m_empty);
    setStyleSheet(styleSheet() + QStringLiteral(
        "QWidget#ListSearchRow{background:transparent;border:0;}"
        "QLabel#ListSearchIcon{background:transparent;border:0;}"
        "QLineEdit#ListSearch{background:transparent;border:0;border-radius:0;padding:0;}"
        "QLineEdit#ListSearch:hover,QLineEdit#ListSearch:focus{background:transparent;border:0;}"));
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString&) { filterLists(); });
    connect(this, &QMenu::aboutToShow, this, [this]() {
        QTimer::singleShot(0, m_search, [this]() { m_search->setFocus(Qt::TabFocusReason); });
    });
}

QAction* ListDestinationMenu::addDestination(QMenu* menu, const TaskList& list,
                                             const QString& path, bool searchResult) {
    auto* action = new QAction(ListAppearance::icon(list, 18,
        ThemeManager::instance().color(ds::ColorToken::TextSecondary)),
        searchResult && !path.isEmpty() ? QStringLiteral("%1 — %2").arg(list.name, path) : list.name, menu);
    action->setObjectName(QStringLiteral("listDestination.%1%2")
        .arg(searchResult ? QStringLiteral("search.") : QString(), list.id));
    action->setProperty("targetListId", list.id);
    action->setProperty("targetListName", list.name);
    action->setProperty("targetFolderName", path);
    action->setCheckable(true);
    action->setChecked(list.id == m_selectedId);
    action->setToolTip(path.isEmpty() ? list.name : QStringLiteral("%1 / %2").arg(path, list.name));
    menu->addAction(action);
    connect(action, &QAction::triggered, this, [this, id = list.id]() {
        hide();
        emit listSelected(id);
    });
    return action;
}

void ListDestinationMenu::setLists(const QVector<TaskList>& inputLists,
                                   const QVector<Folder>& inputFolders,
                                   const QString& selectedId) {
    for (QAction* action : m_rootActions) {
        if (!action) continue;
        removeAction(action);
        // 文件夹的 menuAction 由子菜单拥有并随子菜单销毁；普通清单动作才由本菜单销毁。
        if (action->parent() == this) delete action;
    }
    for (QAction* action : m_searchActions) { removeAction(action); delete action; }
    m_rootActions.clear();
    m_searchActions.clear();
    for (const QPointer<PopupMenu>& menu : m_folderMenus) if (menu) {
        menu->hide();
        // 延迟销毁前先脱离对象树，避免重建期间 findChild 命中上一轮同名菜单及其动作。
        menu->setParent(nullptr);
        menu->deleteLater();
    }
    m_folderMenus.clear();
    m_selectedId = selectedId;
    QVector<TaskList> lists;
    for (const TaskList& list : inputLists) if (list.archivedAtUtcMs == 0) lists.append(list);
    QVector<Folder> folders = inputFolders;
    const auto ordered = [](const TaskList& a, const TaskList& b) {
        return a.sortOrder != b.sortOrder ? a.sortOrder < b.sortOrder
            : QString::localeAwareCompare(a.name, b.name) < 0;
    };
    std::sort(lists.begin(), lists.end(), ordered);
    std::sort(folders.begin(), folders.end(), [](const Folder& a, const Folder& b) {
        return a.sortOrder != b.sortOrder ? a.sortOrder < b.sortOrder
            : QString::localeAwareCompare(a.name, b.name) < 0;
    });
    for (const Folder& folder : folders) {
        auto* child = new PopupMenu(folder.name, this);
        child->setObjectName(QStringLiteral("listFolderMenu.%1").arg(folder.id));
        child->setProperty("entityMenu", true);
        child->setMinimumWidth(224);
        bool empty = true;
        for (const TaskList& list : lists) if (list.folderId == folder.id) {
            addDestination(child, list, folder.name, false);
            empty = false;
        }
        if (empty) {
            QAction* action = child->addAction(QStringLiteral("空文件夹"));
            action->setEnabled(false);
        }
        QAction* folderAction = QMenu::addMenu(child);
        folderAction->setIcon(AppIcons::icon(IconId::Folder, 18));
        folderAction->setProperty("folderId", folder.id);
        m_rootActions.append(folderAction);
        m_folderMenus.append(child);
    }
    for (const TaskList& list : lists) if (list.folderId.isEmpty())
        m_rootActions.append(addDestination(this, list, QString(), false));
    for (const TaskList& list : lists) {
        QString path;
        for (const Folder& folder : folders) if (folder.id == list.folderId) path = folder.name;
        m_searchActions.append(addDestination(this, list, path, true));
    }
    insertAction(m_empty, nullptr);
    m_search->clear();
    filterLists();
}

void ListDestinationMenu::filterLists() {
    const QString query = m_search->text().trimmed();
    for (const QPointer<PopupMenu>& menu : m_folderMenus) if (menu && !query.isEmpty()) menu->hide();
    for (QAction* action : m_rootActions) action->setVisible(query.isEmpty());
    int matches = 0;
    for (QAction* action : m_searchActions) {
        const bool visible = !query.isEmpty()
            && (action->property("targetListName").toString().contains(query, Qt::CaseInsensitive)
                || action->property("targetFolderName").toString().contains(query, Qt::CaseInsensitive));
        action->setVisible(visible);
        if (visible) ++matches;
    }
    m_empty->setVisible(!query.isEmpty() && matches == 0);
}

bool ListDestinationMenu::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_search && event->type() == QEvent::KeyPress) {
        const QKeyEvent* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up
            || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            const QVector<QAction*> candidates = m_search->text().trimmed().isEmpty()
                ? m_rootActions : m_searchActions;
            for (QAction* action : candidates) if (action->isVisible() && action->isEnabled()) {
                if (key->key() != Qt::Key_Down && key->key() != Qt::Key_Up
                    && !action->property("targetListId").toString().isEmpty()) {
                    action->trigger();
                } else {
                    setActiveAction(action);
                    setFocus(Qt::TabFocusReason);
                }
                return true;
            }
            return key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
        }
    }
    return PopupMenu::eventFilter(watched, event);
}

void ListDestinationMenu::popupAbove(QWidget* anchor) {
    if (!anchor) return;
    ensurePolished();
    const QSize size = sizeHint();
    const int margin = PopupMenu::shadowMargin();
    const QSize cardSize(size.width() - 2 * margin, size.height() - 2 * margin);
    const QPoint top = anchor->mapToGlobal(QPoint(0, 0));
    QPoint position(top.x(), top.y() - cardSize.height() - 8);
    if (QScreen* screen = QGuiApplication::screenAt(top)) {
        const QRect area = screen->availableGeometry();
        position.setX(qBound(area.left(), position.x(),
                             qMax(area.left(), area.right() - cardSize.width() + 1)));
        if (position.y() < area.top())
            position.setY(anchor->mapToGlobal(QPoint(0, anchor->height())).y() + 8);
    }
    popup(position);
    // QMenu::popup 还会按自身 action/panel 几何二次偏移；返回后统一以可见卡片落位。
    move(position - QPoint(margin, margin));
}

ListSelectPopup::ListSelectPopup(QWidget* parent) : ListDestinationMenu(parent) {
    setObjectName(QStringLiteral("GroupSelectMenu"));
    setWindowTitle(QStringLiteral("所属清单"));
}
} // namespace kl
