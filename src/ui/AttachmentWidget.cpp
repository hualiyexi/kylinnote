#include "AttachmentWidget.h"

#include <QAction>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariant>

#include "../app/TaskStore.h"
#include "AppIcons.h"

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
QString readableSize(qint64 bytes) {
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
}
}  // namespace

AttachmentWidget::AttachmentWidget(TaskStore* store, QWidget* parent) : QWidget(parent), m_store(store) {
    setObjectName(QStringLiteral("AttachmentWidget"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);
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
    m_add->setIcon(AppIcons::icon(IconId::Attachment)); m_add->setIconSize(QSize(16, 16));
    m_add->setAccessibleName(QStringLiteral("添加图片或附件"));
    m_add->setToolTip(QStringLiteral("复制到本机任务目录，单个文件不超过 2 MB"));
    header->addWidget(m_add);
    root->addWidget(m_header);
    m_empty = new QLabel(QStringLiteral("添加参考图片或相关材料，文件会保存在本机。"), this);
    m_empty->setProperty("muted", QVariant(true));
    m_empty->setWordWrap(true);
    root->addWidget(m_empty);
    auto* rowsHost = new QWidget(this);
    auto* rowsLayout = new QVBoxLayout(rowsHost);
    rowsLayout->setContentsMargins(0, 0, 0, 0);
    rowsLayout->setSpacing(6);
    m_rows = rowsLayout;
    root->addWidget(rowsHost);
    connect(m_add, &QPushButton::clicked, this, &AttachmentWidget::importFiles);
    refresh();
}

void AttachmentWidget::setStore(TaskStore* store) {
    if (m_store == store)
        return;
    m_store = store;
    refresh();
}

QString AttachmentWidget::filePath(const Attachment& attachment) const {
    if (!m_store)
        return QString();
    const QString path = m_store->attachmentFilePath(attachment);
    const QFileInfo info(path);
    return info.exists() && info.isFile() ? path : QString();
}

void AttachmentWidget::setTask(QString entityId) {
    m_entityId = entityId;
    refresh();
}

void AttachmentWidget::setReadOnly(bool readOnly) {
    m_readOnly = readOnly;
    refresh();
}

void AttachmentWidget::setCompact(bool compact) {
    m_compact = compact;
}

void AttachmentWidget::clearRows() {
    while (QLayoutItem* child = m_rows->takeAt(0)) {
        if (child->widget())
            child->widget()->deleteLater();
        delete child;
    }
}

void AttachmentWidget::refresh() {
    clearRows();
    const QVector<Attachment> items = m_store ? m_store->attachments(m_entityId) : QVector<Attachment>();
    m_header->setVisible(!m_compact);
    m_empty->setVisible(items.isEmpty() && !m_entityId.isEmpty() && !m_compact);
    setVisible(!m_entityId.isEmpty() && (!m_compact || !items.isEmpty()));
    m_add->setEnabled(!m_entityId.isEmpty() && !m_readOnly && m_store != nullptr);
    for (const Attachment& attachment : items)
        addRow(attachment);
}

void AttachmentWidget::addRow(const Attachment& attachment) {
    auto* row = new QWidget(this);
    row->setObjectName(QStringLiteral("AttachmentRow"));
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    auto* preview = new QLabel(row);
    preview->setObjectName(QStringLiteral("AttachmentThumbnail"));
    preview->setFixedSize(m_compact ? 52 : 64, m_compact ? 40 : 48);
    preview->setAlignment(Qt::AlignCenter);
    preview->setCursor(Qt::PointingHandCursor);
    if (attachment.isImage) {
        QPixmap image(filePath(attachment));
        if (!image.isNull()) {
            preview->setPixmap(
                image.scaled(preview->size() - QSize(2, 2), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            preview->setText(QStringLiteral("图片"));
        }
    } else {
        preview->setText(QFileInfo(attachment.originalName).suffix().toUpper().left(4));
    }
    preview->setProperty("fileAttachment", QVariant(!attachment.isImage));
    layout->addWidget(preview);

    auto* text = new QVBoxLayout;
    text->setSpacing(2);
    auto* name = new QLabel(attachment.originalName, row);
    name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    text->addWidget(name);
    auto* detail = new QLabel(readableSize(attachment.size), row);
    detail->setProperty("muted", QVariant(true));
    text->addWidget(detail);
    layout->addLayout(text, 1);

    auto* actions = new QToolButton(row);
    actions->setObjectName(QStringLiteral("AttachmentActions")); actions->setText(QStringLiteral("附件操作"));
    actions->setIcon(AppIcons::icon(IconId::More)); actions->setIconSize(QSize(16, 16));
    actions->setToolButtonStyle(Qt::ToolButtonIconOnly); actions->setAutoRaise(true);
    actions->setToolTip(QStringLiteral("附件操作")); actions->setAccessibleName(QStringLiteral("附件操作"));
    actions->setFocusPolicy(Qt::NoFocus); actions->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(actions); menu->setObjectName(QStringLiteral("AttachmentMenu"));
    auto* open = menu->addAction(QStringLiteral("打开")); open->setObjectName(QStringLiteral("OpenAttachment"));
    auto* remove = menu->addAction(AppIcons::icon(IconId::Delete), QStringLiteral("移除"));
    remove->setObjectName(QStringLiteral("RemoveAttachment")); remove->setVisible(!m_readOnly && !m_compact);
    actions->setMenu(menu); layout->addWidget(actions);
    if (m_compact) {
        name->setToolTip(attachment.originalName);
        name->setMaximumWidth(190);
    }
    m_rows->addWidget(row);

    connect(open, &QAction::triggered, this, [this, attachment]() {
        if (attachment.isImage) {
            ImagePreviewDialog dialog(attachment, filePath(attachment), this);
            dialog.exec();
        } else {
            m_store->openAttachment(attachment.id);
        }
    });
    connect(remove, &QAction::triggered, this, [this, attachment]() {
        if (QMessageBox::question(
                this,
                QStringLiteral("移除附件"),
                QStringLiteral("从任务移除“%1”？\n仅删除应用中的副本，不影响你选择的原文件。")
                    .arg(attachment.originalName)) == QMessageBox::Yes)
            m_store->removeAttachment(m_entityId, attachment.id);
    });
}

void AttachmentWidget::importFiles() {
    if (m_entityId.isEmpty() || m_readOnly)
        return;
    const QStringList paths = QFileDialog::getOpenFileNames(
        this,
        QStringLiteral("添加图片或附件（单个不超过 2 MB）"),
        QString(),
        QStringLiteral("图片与常用文件 (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.txt *.md *.pdf *.doc *.docx "
                       "*.xls *.xlsx);;所有文件 (*)"));
    for (const QString& path : paths) {
        if (m_store->importAttachment(m_entityId, path).id.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("无法添加附件"), m_store->lastError());
            break;
        }
    }
}

}  // namespace kl
