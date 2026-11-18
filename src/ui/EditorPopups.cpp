#include "EditorPopups.h"

#include <QApplication>
#include <QCompleter>
#include <QCursor>
#include <QEvent>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "../domain/InternalLink.h"
#include "AppIcons.h"
#include "PopupShadow.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {
namespace {
constexpr int kRowHeight = 34;
constexpr int kCardPadding = 4;
constexpr int kValueRole = Qt::UserRole + 1;
constexpr int kCreateRole = Qt::UserRole + 2;

QColor color(ds::ColorToken token) { return ThemeManager::instance().color(token); }
QString cssColor(ds::ColorToken token) { return color(token).name(QColor::HexArgb); }

QRect placeCard(QSize size, const QRect& anchor, const QRect& bounds) {
    size.setWidth(qMin(size.width(), bounds.width()));
    size.setHeight(qMin(size.height(), bounds.height()));
    const int x = qBound(bounds.left(), anchor.left(), bounds.right() - size.width() + 1);
    int y = anchor.bottom() + 5;
    if (y + size.height() - 1 > bounds.bottom()) y = anchor.top() - size.height() - 4;
    y = qBound(bounds.top(), y, bounds.bottom() - size.height() + 1);
    return QRect(QPoint(x, y), size);
}

void paintCard(QPainter& painter, const QRect& rect, int radius) {
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(rect).adjusted(.5, .5, -.5, -.5), radius, radius);
    painter.fillPath(path, color(ds::ColorToken::MenuBg));
    painter.setPen(QPen(color(ds::ColorToken::Border), 1));
    painter.drawPath(path);
}

void paintSurface(QPainter& painter, const QRect& rect, int radius, QImage& shadow) {
    const QSize size(painter.device()->width(), painter.device()->height());
    if (shadow.size() != size)
        shadow = popupShadow(size, rect, radius, color(ds::ColorToken::Shadow));
    painter.drawImage(0, 0, shadow);
    paintCard(painter, rect, radius);
}

class HoverShadow : public QWidget {
public:
    explicit HoverShadow(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("LinkHoverShadow"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        const QColor current = color(ds::ColorToken::Shadow);
        if (m_shadow.size() != size() || m_color != current) {
            m_color = current;
            m_shadow = popupShadow(size(), rect().adjusted(PopupShadowMargin, PopupShadowMargin,
                -PopupShadowMargin, -PopupShadowMargin), 6, current);
        }
        QPainter painter(this); painter.drawImage(0, 0, m_shadow);
    }
private:
    QImage m_shadow;
    QColor m_color;
};

class HintDelegate : public QStyledItemDelegate {
public:
    explicit HintDelegate(QObject* parent) : QStyledItemDelegate(parent) {}
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return QSize(180, kRowHeight); }
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        if ((option.state & QStyle::State_Enabled)
            && (option.state & (QStyle::State_Selected | QStyle::State_MouseOver))) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(color(ds::ColorToken::MenuHover));
            painter->drawRoundedRect(option.rect, 8, 8);
        }
        painter->setFont(ds::qfont(ds::FontScene::Menu));
        painter->setPen(color(index.flags() & Qt::ItemIsEnabled ? ds::ColorToken::Text : ds::ColorToken::TextTertiary));
        const QRect text = option.rect.adjusted(12, 0, -12, 0);
        const QString label = painter->fontMetrics().elidedText(index.data().toString(), Qt::ElideRight, text.width());
        painter->drawText(text, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine, label);
        painter->restore();
    }
};

class HintView : public QListView {
public:
    explicit HintView(QWidget* parent = nullptr) : QListView(parent) {
        setObjectName(QStringLiteral("EditorHintList"));
        setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFocusPolicy(Qt::NoFocus);
        setFrameShape(QFrame::NoFrame);
        setMouseTracking(true);
        setUniformItemSizes(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setItemDelegate(new HintDelegate(this));
        setViewportMargins(PopupShadowMargin + kCardPadding, PopupShadowMargin + kCardPadding,
                           PopupShadowMargin + kCardPadding, PopupShadowMargin + kCardPadding);
        viewport()->setAutoFillBackground(false);
        setStyleSheet(QStringLiteral("QListView#EditorHintList{background:transparent;border:0;padding:0;}"));
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this](ds::Theme) {
            m_shadow = QImage(); update(); viewport()->update();
        });
    }
protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::Paint) {
            QPainter painter(this);
            paintSurface(painter, rect().adjusted(PopupShadowMargin, PopupShadowMargin,
                                                  -PopupShadowMargin, -PopupShadowMargin), 12, m_shadow);
            return true;
        }
        return QListView::event(event);
    }
    void resizeEvent(QResizeEvent* event) override {
        QListView::resizeEvent(event);
        verticalScrollBar()->setGeometry(width() - PopupShadowMargin - 7, PopupShadowMargin + kCardPadding,
                                         6, qMax(0, height() - 2 * (PopupShadowMargin + kCardPadding)));
        m_shadow = QImage();
    }
private:
    QImage m_shadow;
};
} // namespace

EditorCompletion::EditorCompletion(QTextEdit* editor, QObject* parent) : QObject(parent), m_editor(editor) {
    m_model = new QStandardItemModel(this);
    m_completer = new QCompleter(m_model, this);
    m_completer->setWidget(editor);
    m_completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    m_completer->setMaxVisibleItems(8);
    auto* view = new HintView;
    view->setProperty("markdownEditor", QVariant::fromValue<QObject*>(parent));
    m_view = view;
    m_completer->setPopup(view);
    m_view->installEventFilter(this);
    editor->installEventFilter(this);
    connect(m_completer, QOverload<const QModelIndex&>::of(&QCompleter::activated), this,
            [this](const QModelIndex& index) {
        if (!index.isValid() || !(index.flags() & Qt::ItemIsEnabled)) return;
        emit chosen(index.data(kValueRole).toString(), index.data(kCreateRole).toBool());
    });
}

bool EditorCompletion::eventFilter(QObject* watched, QEvent* event) {
    if ((watched != m_editor && watched != m_view) || !isVisible()) return QObject::eventFilter(watched, event);
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride)
        return QObject::eventFilter(watched, event);
    auto* key = static_cast<QKeyEvent*>(event);
    const int code = key->key();
    if (code != Qt::Key_Escape && code != Qt::Key_Return && code != Qt::Key_Enter
        && code != Qt::Key_Up && code != Qt::Key_Down) return QObject::eventFilter(watched, event);
    key->accept();
    if (event->type() == QEvent::ShortcutOverride) return true;
    if (code == Qt::Key_Escape) {
        emit dismissed(); hide(); return true;
    }
    if (code == Qt::Key_Up || code == Qt::Key_Down) {
        const int rows = m_view->model()->rowCount();
        if (rows == 0) return true;
        const int current = m_view->currentIndex().isValid() ? m_view->currentIndex().row() : 0;
        const int row = qBound(0, current + (code == Qt::Key_Down ? 1 : -1), rows - 1);
        m_view->setCurrentIndex(m_view->model()->index(row, 0));
        m_view->scrollTo(m_view->currentIndex());
        return true;
    }
    const QModelIndex index = m_view->currentIndex();
    if (index.isValid() && (index.flags() & Qt::ItemIsEnabled)) {
        const QString value = index.data(kValueRole).toString();
        const bool create = index.data(kCreateRole).toBool();
        if (!value.isEmpty()) emit chosen(value, create);
    }
    hide();
    return true;
}

void EditorCompletion::showItems(const QVector<EditorHint>& items, const QRect& anchor,
                                 const QRect& bounds, const QString& accessibleName) {
    if (!m_editor || !m_editor->isVisible() || bounds.height() < kRowHeight + 8 || bounds.width() < 120) {
        hide(); return;
    }
    const QString previous = isVisible() ? m_view->currentIndex().data(kValueRole).toString() : QString();
    m_model->clear();
    for (const EditorHint& hint : items) {
        auto* item = new QStandardItem(hint.text);
        item->setData(hint.value, kValueRole);
        item->setData(hint.create, kCreateRole);
        item->setToolTip(hint.toolTip.isEmpty() ? hint.text : hint.toolTip);
        if (hint.value.isEmpty() && !hint.create) item->setFlags(Qt::NoItemFlags);
        m_model->appendRow(item);
    }
    if (m_model->rowCount() == 0) { hide(); return; }
    m_view->setAccessibleName(accessibleName);
    const int natural = qMin(8, m_model->rowCount()) * kRowHeight + 2 * kCardPadding;
    const QRect card = placeCard(QSize(qMin(220, bounds.width()), natural), anchor, bounds);
    m_view->setFixedSize(card.width() + 2 * PopupShadowMargin, card.height() + 2 * PopupShadowMargin);
    QRect local(m_editor->mapFromGlobal(card.topLeft() - QPoint(PopupShadowMargin, PopupShadowMargin)), QSize(card.width(), 1));
    m_completer->setCompletionPrefix(QString());
    m_completer->complete(local);
    m_view->move(card.topLeft() - QPoint(PopupShadowMargin, PopupShadowMargin));
    int row = 0;
    if (!previous.isEmpty()) {
        for (int i = 0; i < m_model->rowCount(); ++i)
            if (m_model->index(i, 0).data(kValueRole).toString() == previous) { row = i; break; }
    }
    m_completer->setCurrentRow(row);
    m_view->setCurrentIndex(m_completer->completionModel()->index(row, 0));
}

void EditorCompletion::hide() { m_view->hide(); }
bool EditorCompletion::isVisible() const { return m_view->isVisible(); }
QWidget* EditorCompletion::popup() const { return m_view; }

LinkTools::LinkTools(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("LinkHoverTools"));
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(70, 36);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(5, 6, 5, 6);
    layout->setSpacing(0);
    const auto button = [this, layout](IconId icon, const QString& name) {
        auto* value = new QToolButton(this);
        value->setObjectName(name);
        value->setProperty("iconId", static_cast<int>(icon));
        value->setAccessibleName(AppIcons::accessibleName(icon));
        value->setToolTip(value->accessibleName());
        value->setAutoRaise(true);
        value->setFocusPolicy(Qt::NoFocus);
        value->setFixedSize(30, 24);
        value->setIconSize(QSize(18, 18));
        value->setCursor(Qt::PointingHandCursor);
        layout->addWidget(value);
        return value;
    };
    connect(button(IconId::EditLink, QStringLiteral("LinkHoverEdit")), &QToolButton::clicked,
            this, &LinkTools::editRequested);
    connect(button(IconId::CopyLink, QStringLiteral("LinkHoverCopy")), &QToolButton::clicked,
            this, &LinkTools::copyRequested);
    m_hideTimer = new QTimer(this);
    m_hideTimer->setSingleShot(true);
    m_hideTimer->setInterval(220);
    connect(m_hideTimer, &QTimer::timeout, this, &QWidget::hide);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this](ds::Theme) { applyTheme(); });
    applyTheme();
    hide();
}

LinkTools::~LinkTools() { delete m_shadowLayer; }

void LinkTools::applyTheme() {
    setStyleSheet(QStringLiteral("QFrame#LinkHoverTools QToolButton{background:transparent;border:0;padding:0;border-radius:4px;}"
                                 "QFrame#LinkHoverTools QToolButton:hover{background:%1;}").arg(cssColor(ds::ColorToken::MenuHover)));
    for (QToolButton* button : findChildren<QToolButton*>())
        button->setIcon(AppIcons::icon(static_cast<IconId>(button->property("iconId").toInt()), ds::ColorToken::TextSecondary, 18));
    if (m_shadowLayer) m_shadowLayer->update();
    update();
}

void LinkTools::showAt(const QRect& anchor, const QRect& bounds) {
    if (!parentWidget() || bounds.width() < 70 || bounds.height() < 36) { hide(); return; }
    m_hideTimer->stop();
    const QRect card = placeCard(QSize(70, 36), anchor, bounds);
    if (!m_shadowLayer) m_shadowLayer = new HoverShadow(parentWidget());
    else if (m_shadowLayer->parentWidget() != parentWidget()) m_shadowLayer->setParent(parentWidget());
    m_shadowLayer->setGeometry(QRect(parentWidget()->mapFromGlobal(card.topLeft() - QPoint(PopupShadowMargin, PopupShadowMargin)),
                                    card.size() + QSize(2 * PopupShadowMargin, 2 * PopupShadowMargin)));
    move(parentWidget()->mapFromGlobal(card.topLeft()));
    m_shadowLayer->show(); m_shadowLayer->raise();
    show(); raise();
}
void LinkTools::scheduleHide() { if (isVisible()) m_hideTimer->start(); }
QRect LinkTools::cardGeometryGlobal() const {
    return QRect(mapToGlobal(QPoint()), size());
}
void LinkTools::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    paintCard(painter, rect(), 6);
}
void LinkTools::hideEvent(QHideEvent* event) {
    if (m_shadowLayer) m_shadowLayer->hide();
    QFrame::hideEvent(event);
}
void LinkTools::enterEvent(QEvent* event) { m_hideTimer->stop(); QFrame::enterEvent(event); }
void LinkTools::leaveEvent(QEvent* event) { scheduleHide(); QFrame::leaveEvent(event); }

LinkEditorPopup::LinkEditorPopup(QWidget* parent)
    : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    setObjectName(QStringLiteral("LinkEditorPopup"));
    setWindowTitle(QStringLiteral("编辑链接"));
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(false);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(PopupShadowMargin, PopupShadowMargin, PopupShadowMargin, PopupShadowMargin);
    outer->setSpacing(0);
    m_card = new QWidget(this);
    m_card->setObjectName(QStringLiteral("LinkEditorCard"));
    outer->addWidget(m_card);
    auto* layout = new QVBoxLayout(m_card);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(0);
    auto* header = new QHBoxLayout;
    auto* title = new QLabel(QStringLiteral("编辑链接"), m_card);
    title->setObjectName(QStringLiteral("LinkEditorTitle"));
    QFont titleFont = ds::qfont(ds::FontScene::Body);
    titleFont.setPointSizeF(ds::pointSize(ds::TypeScale::Subtitle));
    titleFont.setBold(true);
    title->setFont(titleFont);
    header->addWidget(title, 1);
    auto* close = new QToolButton(m_card);
    close->setObjectName(QStringLiteral("LinkEditorClose"));
    close->setAccessibleName(QStringLiteral("关闭编辑链接"));
    close->setAutoRaise(true);
    close->setFixedSize(24, 24);
    close->setIconSize(QSize(14, 14));
    header->addWidget(close);
    connect(close, &QToolButton::clicked, this, &QDialog::reject);
    layout->addLayout(header);
    layout->addSpacing(12);
    m_text = new QLineEdit(m_card);
    m_text->setObjectName(QStringLiteral("LinkText"));
    m_text->setPlaceholderText(QStringLiteral("文本"));
    m_text->setAccessibleName(QStringLiteral("链接文字"));
    m_text->setFont(ds::qfont(ds::FontScene::Body));
    m_text->setFixedHeight(36);
    layout->addWidget(m_text);
    layout->addSpacing(12);
    m_target = new QLineEdit(m_card);
    m_target->setObjectName(QStringLiteral("LinkTarget"));
    m_target->setPlaceholderText(QStringLiteral("链接"));
    m_target->setAccessibleName(QStringLiteral("链接地址或关联任务/笔记标识"));
    m_target->setFont(ds::qfont(ds::FontScene::Body));
    m_target->setFixedHeight(36);
    layout->addWidget(m_target);
    layout->addStretch(1);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(12);
    buttons->addStretch(1);
    m_confirm = new QPushButton(QStringLiteral("确定"), m_card);
    m_confirm->setObjectName(QStringLiteral("LinkConfirm"));
    m_confirm->setFixedSize(88, 30);
    m_confirm->setDefault(true);
    buttons->addWidget(m_confirm);
    m_remove = new QPushButton(QStringLiteral("删除链接"), m_card);
    m_remove->setObjectName(QStringLiteral("LinkRemove"));
    m_remove->setFixedSize(88, 30);
    buttons->addWidget(m_remove);
    layout->addLayout(buttons);
    connect(m_target, &QLineEdit::textChanged, this, &LinkEditorPopup::validate);
    connect(m_confirm, &QPushButton::clicked, this, [this]() {
        if (!isSafeLinkTarget(m_target->text().trimmed())) return;
        emit submitted(m_text->text(), m_target->text().trimmed(), false);
        accept();
    });
    connect(m_remove, &QPushButton::clicked, this, [this]() {
        emit submitted(m_text->text(), m_target->text().trimmed(), true);
        accept();
    });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this](ds::Theme) { applyTheme(); });
    applyTheme();
}

void LinkEditorPopup::applyTheme() {
    setStyleSheet(QStringLiteral(
        "QWidget#LinkEditorCard{background:transparent;}"
        "QWidget#LinkEditorCard QLabel{color:%1;}"
        "QWidget#LinkEditorCard QLineEdit{background:transparent;color:%1;border:1px solid %2;border-radius:6px;padding:0 12px;}"
        "QWidget#LinkEditorCard QLineEdit:focus{border-color:%3;}"
        "QWidget#LinkEditorCard QToolButton{background:transparent;border:0;padding:0;}"
        "QPushButton#LinkConfirm{background:%3;color:%4;border:0;border-radius:8px;padding:0;}"
        "QPushButton#LinkConfirm:disabled{background:%2;}"
        "QPushButton#LinkRemove{background:transparent;color:%5;border:1px solid %2;border-radius:8px;padding:0;}"
        "QPushButton#LinkRemove:hover{background:%6;}")
        .arg(cssColor(ds::ColorToken::Text), cssColor(ds::ColorToken::Border), cssColor(ds::ColorToken::Primary),
             cssColor(ds::ColorToken::OnPrimary), cssColor(ds::ColorToken::TextSecondary), cssColor(ds::ColorToken::MenuHover)));
    findChild<QToolButton*>(QStringLiteral("LinkEditorClose"))->setIcon(AppIcons::icon(IconId::Close, ds::ColorToken::TextSecondary, 14));
    m_shadow = QImage(); update();
}

void LinkEditorPopup::validate() {
    const bool valid = isSafeLinkTarget(m_target->text().trimmed());
    m_confirm->setEnabled(valid);
    m_target->setToolTip(valid ? QString() : QStringLiteral("请输入有效的 http、https、mailto 地址或任务/笔记标识"));
}

void LinkEditorPopup::openFor(const QString& label, const QString& target, bool existing,
                              const QRect& anchor, const QRect& bounds) {
    if (bounds.width() < 220 || bounds.height() < 212) return;
    m_text->setText(label);
    m_target->setText(target);
    m_remove->setVisible(existing);
    const QRect card = placeCard(QSize(296, 212), anchor, bounds);
    setFixedSize(card.width() + 2 * PopupShadowMargin, card.height() + 2 * PopupShadowMargin);
    move(card.topLeft() - QPoint(PopupShadowMargin, PopupShadowMargin));
    validate();
    show(); raise(); activateWindow();
    m_target->setFocus();
    m_target->setCursorPosition(m_target->text().size());
}

void LinkEditorPopup::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    paintSurface(painter, rect().adjusted(PopupShadowMargin, PopupShadowMargin, -PopupShadowMargin, -PopupShadowMargin), 12, m_shadow);
}

} // namespace kl
