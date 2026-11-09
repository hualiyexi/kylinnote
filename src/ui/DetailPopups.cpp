#include "DetailPopups.h"

#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QScreen>
#include <QTextBoundaryFinder>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
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
    QColor tint = theme.color(ds::ColorToken::Text);
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
                                  const QString& plainBody, const QStringList& checklist) {
    const QString text = title + QLatin1Char('\n') + plainBody + QLatin1Char('\n') + checklist.join(QLatin1Char('\n'));
    QString characters = text;
    characters.remove(QLatin1Char('\r'));
    characters.remove(QLatin1Char('\n'));
    m_words->setNum(wordCount(text));
    m_words->setToolTip(QStringLiteral("标题、正文与检查项中的中文字符及英文单词，不计格式标记与标点"));
    m_characters->setNum(characters.toUcs4().size());
    m_characters->setToolTip(QStringLiteral("标题、正文与检查项中的字符，含空格和标点，不含换行与格式标记"));
    m_created->setText(displayDate(entity.createdAt));
    m_updated->setText(displayDate(entity.updatedAt));
    m_created->setToolTip(toAppTime(entity.createdAt).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    m_updated->setToolTip(toAppTime(entity.updatedAt).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
}

ListSelectPopup::ListSelectPopup(QWidget* parent) : DetailPopup(parent) {
    setObjectName(QStringLiteral("GroupSelectMenu"));
    setWindowTitle(QStringLiteral("所属清单"));
    card()->setFixedWidth(240);
    auto* contents = new QVBoxLayout(card());
    contents->setContentsMargins(8, 6, 8, 8);
    contents->setSpacing(4);
    m_search = new QLineEdit(card());
    m_search->setObjectName(QStringLiteral("ListSearch"));
    m_search->setPlaceholderText(QStringLiteral("搜索清单"));
    m_search->setAccessibleName(QStringLiteral("搜索清单"));
    m_search->addAction(AppIcons::icon(IconId::Search), QLineEdit::LeadingPosition);
    contents->addWidget(m_search);
    m_tree = new QTreeWidget(card());
    m_tree->setObjectName(QStringLiteral("ListSelectTree"));
    m_tree->setHeaderHidden(true);
    m_tree->setColumnCount(2);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_tree->setColumnWidth(1, 24);
    m_tree->setIndentation(14);
    m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    contents->addWidget(m_tree);
    m_empty = new QLabel(QStringLiteral("没有匹配的清单"), card());
    m_empty->setProperty("muted", true);
    m_empty->setContentsMargins(8, 10, 8, 10);
    m_empty->hide();
    contents->addWidget(m_empty);
    connect(m_search, &QLineEdit::textChanged, this, &ListSelectPopup::filterLists);
    const auto select = [this](QTreeWidgetItem* item) {
        const QString id = item->data(0, Qt::UserRole).toString();
        if (id.isEmpty()) {
            item->setExpanded(!item->isExpanded());
            return;
        }
        hide();
        emit listSelected(id);
    };
    connect(m_tree, &QTreeWidget::itemClicked, this, [select](QTreeWidgetItem* item, int) { select(item); });
    connect(m_tree, &QTreeWidget::itemActivated, this, [select](QTreeWidgetItem* item, int) { select(item); });
    connect(m_search, &QLineEdit::returnPressed, this, [this, select]() {
        for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
            if (!(*it)->isHidden() && !(*it)->data(0, Qt::UserRole).toString().isEmpty()) {
                select(*it);
                break;
            }
        }
    });
}

void ListSelectPopup::setLists(const QVector<TaskList>& lists, const QVector<Folder>& folders,
                              const QString& selectedId) {
    m_tree->clear();
    m_search->clear();
    QHash<QString, QTreeWidgetItem*> parents;
    for (const Folder& folder : folders) {
        auto* item = new QTreeWidgetItem(m_tree, {folder.name});
        item->setIcon(0, AppIcons::icon(IconId::Folder));
        item->setExpanded(true);
        parents.insert(folder.id, item);
    }
    for (const TaskList& list : lists) {
        auto* item = parents.contains(list.folderId)
            ? new QTreeWidgetItem(parents.value(list.folderId), {list.name})
            : new QTreeWidgetItem(m_tree, {list.name});
        const IconId icon = list.inbox ? IconId::Inbox : (list.kind == EntityKind::Note ? IconId::Note : IconId::TaskList);
        item->setIcon(0, AppIcons::icon(icon));
        item->setData(0, Qt::UserRole, list.id);
        item->setToolTip(0, list.name);
        if (list.id == selectedId) {
            item->setIcon(1, AppIcons::icon(IconId::Check, ds::ColorToken::Primary));
            m_tree->setCurrentItem(item);
        }
    }
    m_tree->setFixedHeight(qBound(64, (lists.size() + folders.size()) * 32 + 4, 256));
    filterLists(QString());
}

void ListSelectPopup::filterLists(const QString& query) {
    bool any = false;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        auto* item = m_tree->topLevelItem(i);
        const bool folderMatch = item->text(0).contains(query, Qt::CaseInsensitive);
        bool visible = folderMatch;
        if (item->childCount()) {
            visible = false;
            for (int j = 0; j < item->childCount(); ++j) {
                auto* child = item->child(j);
                const bool match = folderMatch || child->text(0).contains(query, Qt::CaseInsensitive);
                child->setHidden(!match);
                visible = visible || match;
            }
            if (!query.isEmpty()) item->setExpanded(true);
        }
        item->setHidden(!visible);
        any = any || visible;
    }
    m_tree->setVisible(any);
    m_empty->setVisible(!any);
}

void ListSelectPopup::popupAbove(QWidget* anchor) {
    popupAt(anchor, true);
    m_search->setFocus();
}
} // namespace kl
