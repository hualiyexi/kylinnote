#include "AttachmentWidget.h"

#include <QAbstractButton>
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QStyle>
#include <QStyleOptionFocusRect>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>

#include "../app/TaskStore.h"
#include "AppIcons.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {
namespace {
// 图片预览只使用应用已经复制并解码过的受管文件。
class ImagePreviewDialog final : public QDialog {
public:
    explicit ImagePreviewDialog(const Attachment& attachment, const QString& path, QWidget* parent)
        : QDialog(parent) {
        setWindowTitle(attachment.originalName);
        resize(860, 660);
        auto* layout = new QVBoxLayout(this);
        auto* label = new QLabel(this);
        label->setAlignment(Qt::AlignCenter);
        label->setMinimumSize(480, 360);
        layout->addWidget(label, 1);
        const QPixmap image(path);
        label->setPixmap(image.scaled(label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    }
};

// 保留完整的文件名给提示和辅助功能，显示文本随行宽实时省略。
class AttachmentNameLabel final : public QLabel {
public:
    explicit AttachmentNameLabel(const QString& name, QWidget* parent) : QLabel(parent) {
        QString displayName = name;
        displayName.replace(QLatin1Char('\n'), QLatin1Char(' '));
        displayName.replace(QLatin1Char('\r'), QLatin1Char(' '));
        displayName.replace(QLatin1Char('\t'), QLatin1Char(' '));
        setText(displayName);
        setTextFormat(Qt::PlainText);
        setToolTip(name);
        setAccessibleName(name);
        setFont(ds::qfont(ds::FontScene::Body));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setMinimumWidth(0);
    }

    QSize minimumSizeHint() const override {
        return QSize(0, QLabel::minimumSizeHint().height());
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        drawFrame(&painter);
        style()->drawItemText(&painter, contentsRect(), alignment() | Qt::TextSingleLine,
                              palette(), isEnabled(),
                              fontMetrics().elidedText(text(), Qt::ElideMiddle, contentsRect().width()),
                              QPalette::WindowText);
    }
};

// 行本身是可访问的打开按钮；溢出菜单仍有独立的 Tab 停靠点。
class AttachmentRow final : public QAbstractButton {
public:
    explicit AttachmentRow(QWidget* parent) : QAbstractButton(parent) {
        setObjectName(QStringLiteral("AttachmentRow"));
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setRemoveAction(QAction* action) { m_remove = action; }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::KeypadModifier) {
            if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
                event->accept();
                if (!event->isAutoRepeat()) click();
                return;
            }
            if (event->key() == Qt::Key_Delete && m_remove && m_remove->isEnabled() && m_remove->isVisible()) {
                event->accept();
                if (!event->isAutoRepeat()) m_remove->trigger();
                return;
            }
        }
        QAbstractButton::keyPressEvent(event);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        QStyleOption option;
        option.initFrom(this);
        style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);
        if (hasFocus()) {
            QStyleOptionFocusRect focus;
            focus.initFrom(this);
            focus.rect = contentsRect();
            focus.backgroundColor = palette().color(QPalette::Window);
            style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, &painter, this);
        }
    }

private:
    QPointer<QAction> m_remove;
};

QString readableSize(qint64 bytes) {
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
}

bool sameRows(const QVector<Attachment>& left, const QVector<Attachment>& right) {
    if (left.size() != right.size()) return false;
    for (int i = 0; i < left.size(); ++i) {
        const Attachment& a = left.at(i);
        const Attachment& b = right.at(i);
        if (a.id != b.id || a.entityId != b.entityId || a.originalName != b.originalName ||
            a.relativePath != b.relativePath || a.size != b.size || a.isImage != b.isImage ||
            a.localState != b.localState || a.sha256 != b.sha256)
            return false;
    }
    return true;
}
}  // namespace

AttachmentWidget::AttachmentWidget(TaskStore* store, QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("AttachmentWidget"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    m_header = new QWidget(this);
    auto* header = new QHBoxLayout(m_header);
    header->setContentsMargins(0, 0, 0, 0);
    auto* title = new QLabel(QStringLiteral("图片与附件"), m_header);
    title->setProperty("section", QVariant(true));
    header->addWidget(title);
    header->addStretch();
    m_add = new QPushButton(QStringLiteral("添加图片或附件"), m_header);
    m_add->setObjectName(QStringLiteral("AddAttachment"));
    m_add->setProperty("quiet", QVariant(true));
    m_add->setIcon(AppIcons::icon(IconId::Attachment));
    m_add->setAccessibleName(QStringLiteral("添加图片或附件"));
    m_add->setToolTip(QStringLiteral("复制到本机任务目录，单个文件不超过 2 MB"));
    header->addWidget(m_add);
    root->addWidget(m_header);
    m_empty = new QLabel(QStringLiteral("添加参考图片或相关材料，文件会保存在本机。"), this);
    m_empty->setProperty("muted", QVariant(true));
    m_empty->setWordWrap(true);
    root->addWidget(m_empty);
    m_rowsHost = new QWidget(this);
    m_rowsHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_rows = new QVBoxLayout(m_rowsHost);
    m_rows->setContentsMargins(0, 0, 0, 0);
    root->addWidget(m_rowsHost);
    connect(m_add, &QPushButton::clicked, this, &AttachmentWidget::importFiles);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        m_add->setIcon(AppIcons::icon(IconId::Attachment));
        m_rowsDirty = true;
        refresh();
    });
    if (store)
        setStore(store);
    else
        refresh();
}

void AttachmentWidget::setStore(TaskStore* store) {
    if (m_store == store) return;
    if (m_store) disconnect(m_store.data(), nullptr, this, nullptr);
    m_store = store;
    if (m_store) {
        connect(m_store.data(), &TaskStore::attachmentsChanged, this, [this](const QString& entityId) {
            if (entityId == m_entityId) refresh();
        });
        connect(m_store.data(), &TaskStore::tasksReset, this, &AttachmentWidget::refresh);
        connect(m_store.data(), &QObject::destroyed, this, [this]() {
            m_store = nullptr;
            m_rowsDirty = true;
            refresh();
        });
    }
    m_rowsDirty = true;
    refresh();
}

QString AttachmentWidget::filePath(const Attachment& attachment) const {
    if (!m_store) return QString();
    const QString path = m_store->attachmentFilePath(attachment);
    const QFileInfo info(path);
    return info.exists() && info.isFile() ? path : QString();
}

void AttachmentWidget::setTask(QString entityId) {
    if (m_entityId == entityId) return;
    m_entityId = entityId;
    m_rowsDirty = true;
    refresh();
}

void AttachmentWidget::setReadOnly(bool readOnly) {
    if (m_readOnly == readOnly) return;
    m_readOnly = readOnly;
    m_rowsDirty = true;
    refresh();
}

void AttachmentWidget::setCompact(bool compact) {
    if (m_compact == compact) return;
    m_compact = compact;
    m_rowsDirty = true;
    refresh();
}

void AttachmentWidget::setInlineMode(bool inlineMode) {
    if (m_inlineMode == inlineMode) return;
    m_inlineMode = inlineMode;
    setProperty("inlineMode", QVariant(inlineMode));
    m_rowsDirty = true;
    refresh();
}

QSize AttachmentWidget::sizeHint() const {
    if (m_entityId.isEmpty() || ((m_inlineMode || m_compact) && m_items.isEmpty()))
        return QSize(0, 0);
    return QWidget::sizeHint();
}

QSize AttachmentWidget::minimumSizeHint() const {
    if (m_entityId.isEmpty() || ((m_inlineMode || m_compact) && m_items.isEmpty()))
        return QSize(0, 0);
    return QWidget::minimumSizeHint();
}

void AttachmentWidget::clearRows() {
    while (QLayoutItem* child = m_rows->takeAt(0)) {
        if (QWidget* row = child->widget()) {
            row->hide();
            row->deleteLater();
        }
        delete child;
    }
}

void AttachmentWidget::refresh() {
    const QVector<Attachment> items = m_store && !m_entityId.isEmpty()
        ? m_store->attachments(m_entityId) : QVector<Attachment>();
    const bool itemsChanged = !sameRows(items, m_items);
    if (!m_rowsDirty && !itemsChanged) return;

    clearRows();
    m_items = items;
    const bool rowsOnly = m_inlineMode || m_compact;
    layout()->setSpacing(rowsOnly ? 0 : style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing, nullptr, this));
    m_rows->setSpacing(m_inlineMode ? 0 : style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing, nullptr, this));
    m_header->setVisible(!rowsOnly);
    m_empty->setVisible(items.isEmpty() && !m_entityId.isEmpty() && !rowsOnly);
    m_rowsHost->setVisible(!items.isEmpty());
    m_add->setEnabled(!m_entityId.isEmpty() && !m_readOnly && m_store);
    for (const Attachment& attachment : items) addRow(attachment);
    setVisible(!m_entityId.isEmpty() && (!rowsOnly || !items.isEmpty()));
    m_rowsDirty = false;
    layout()->invalidate();
    updateGeometry();

    const int naturalHeight = sizeHint().height();
    if (m_naturalHeight != naturalHeight) {
        m_naturalHeight = naturalHeight;
        emit heightChanged();
    }
    if (itemsChanged) emit contentChanged();
}

void AttachmentWidget::addRow(const Attachment& attachment) {
    const bool compact = m_compact && !m_inlineMode;
    const bool removable = !m_readOnly && (!m_compact || m_inlineMode);
    const QPointer<TaskStore> store = m_store;
    const QString entityId = m_entityId;
    auto* row = new AttachmentRow(m_rowsHost);
    row->setProperty("inlineAttachment", m_inlineMode);
    row->setText(attachment.originalName);
    row->setToolTip(attachment.originalName);
    row->setAccessibleName(QStringLiteral("打开附件“%1”").arg(attachment.originalName));
    row->setAccessibleDescription(removable
        ? QStringLiteral("按 Enter 或空格打开，按 Delete 移除；Tab 可进入附件操作菜单。")
        : QStringLiteral("按 Enter 或空格打开，Tab 可进入附件操作菜单。"));
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    auto* preview = new QLabel(row);
    preview->setObjectName(compact ? QStringLiteral("AttachmentThumbnail") : QStringLiteral("AttachmentIcon"));
    preview->setAlignment(Qt::AlignCenter);
    preview->setToolTip(attachment.originalName);
    if (compact) {
        // 桌面便签仍保留原有的小缩略图与两行文字。
        preview->setFixedSize(52, 40);
        if (attachment.isImage) {
            const QPixmap image(filePath(attachment));
            if (!image.isNull())
                preview->setPixmap(image.scaled(preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            else
                preview->setText(QStringLiteral("图片"));
        } else {
            preview->setText(QFileInfo(attachment.originalName).suffix().toUpper().left(4));
        }
        preview->setProperty("fileAttachment", QVariant(!attachment.isImage));
    } else {
        const QSize iconSize = m_add->iconSize();
        preview->setFixedSize(iconSize);
        preview->setPixmap(AppIcons::icon(IconId::Attachment).pixmap(iconSize));
    }
    rowLayout->addWidget(preview);

    auto* name = new AttachmentNameLabel(attachment.originalName, row);
    name->setObjectName(QStringLiteral("AttachmentName"));
    auto* detail = new QLabel(readableSize(attachment.size), row);
    detail->setObjectName(QStringLiteral("AttachmentSize"));
    detail->setFont(ds::qfont(ds::FontScene::Caption));
    detail->setProperty("muted", QVariant(true));
    detail->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    if (compact) {
        auto* text = new QVBoxLayout;
        text->setSpacing(0);
        text->addWidget(name);
        text->addWidget(detail);
        rowLayout->addLayout(text, 1);
    } else {
        rowLayout->addWidget(name, 1);
        rowLayout->addWidget(detail);
    }

    auto* actions = new QToolButton(row);
    actions->setObjectName(QStringLiteral("AttachmentActions"));
    actions->setText(QStringLiteral("附件操作"));
    actions->setIcon(AppIcons::icon(IconId::More));
    actions->setIconSize(m_add->iconSize());
    actions->setToolButtonStyle(Qt::ToolButtonIconOnly);
    actions->setAutoRaise(true);
    actions->setToolTip(QStringLiteral("“%1”的附件操作").arg(attachment.originalName));
    actions->setAccessibleName(actions->toolTip());
    actions->setFocusPolicy(Qt::StrongFocus);
    actions->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(actions);
    menu->setObjectName(QStringLiteral("AttachmentMenu"));
    auto* open = menu->addAction(QStringLiteral("打开"));
    open->setObjectName(QStringLiteral("OpenAttachment"));
    auto* remove = menu->addAction(AppIcons::icon(IconId::Delete), QStringLiteral("移除"));
    remove->setObjectName(QStringLiteral("RemoveAttachment"));
    remove->setVisible(removable);
    remove->setEnabled(removable);
    actions->setMenu(menu);
    rowLayout->addWidget(actions);
    row->setRemoveAction(remove);
    row->setContextMenuPolicy(Qt::ActionsContextMenu);
    row->addAction(open);
    row->addAction(remove);
    connect(row, &QAbstractButton::clicked, open, &QAction::trigger);
    m_rows->addWidget(row);
    row->show();

    connect(open, &QAction::triggered, this, [this, attachment, store, entityId]() {
        if (!store || store.data() != m_store.data() || entityId != m_entityId) return;
        if (attachment.isImage) {
            ImagePreviewDialog dialog(attachment, filePath(attachment), this);
            dialog.exec();
        } else {
            store->openAttachment(attachment.id);
        }
    });
    connect(remove, &QAction::triggered, this, [this, attachment, store, entityId]() {
        if (!store || store.data() != m_store.data() || entityId != m_entityId ||
            m_readOnly || (m_compact && !m_inlineMode)) return;
        const auto answer = QMessageBox::question(
            this, QStringLiteral("移除附件"),
            QStringLiteral("从任务移除“%1”？\n仅删除应用中的副本，不影响你选择的原文件。")
                .arg(attachment.originalName));
        // 模态窗口期间仍可能切换任务、存储或只读状态。
        if (answer != QMessageBox::Yes || !store || store.data() != m_store.data() ||
            entityId != m_entityId || m_readOnly || (m_compact && !m_inlineMode)) return;
        store->removeAttachment(entityId, attachment.id);
    });
}

void AttachmentWidget::importFiles() {
    if (!m_store || m_entityId.isEmpty() || m_readOnly) return;
    const QPointer<TaskStore> store = m_store;
    const QString entityId = m_entityId;
    const QStringList paths = QFileDialog::getOpenFileNames(
        this,
        QStringLiteral("添加图片或附件（单个不超过 2 MB）"),
        QString(),
        QStringLiteral("图片与常用文件 (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.txt *.md *.pdf *.doc *.docx "
                       "*.xls *.xlsx);;所有文件 (*)"));
    for (const QString& path : paths) {
        if (!store || store.data() != m_store.data() || entityId != m_entityId || m_readOnly) return;
        if (store->importAttachment(entityId, path).id.isEmpty()) {
            if (store) QMessageBox::warning(this, QStringLiteral("无法添加附件"), store->lastError());
            break;
        }
    }
}

}  // namespace kl
