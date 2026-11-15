#include "FolderEditorDialog.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>

#include "EmojiPickerPopup.h"
#include "GlyphButton.h"
#include "design/ThemeManager.h"

namespace kl {

FolderEditorDialog::FolderEditorDialog(QWidget* parent) : ModalDialog(parent)
{
    setObjectName(QStringLiteral("FolderEditorDialog"));
    setFixedWidth(420);

    auto* nameField = new QFrame(this);
    nameField->setObjectName(QStringLiteral("FolderNameField"));
    nameField->setAttribute(Qt::WA_StyledBackground, true);
    nameField->setFixedHeight(38);
    auto* nameLayout = new QHBoxLayout(nameField);
    nameLayout->setContentsMargins(3, 3, 9, 3);
    nameLayout->setSpacing(5);

    auto* glyph = new GlyphButton(nameField);
    glyph->setObjectName(QStringLiteral("FolderIconButton"));
    glyph->setDefaultIconName(QStringLiteral("folder"));
    glyph->setFixedSize(30, 30);
    glyph->setToolTip(QStringLiteral("选择文件夹图标"));
    glyph->setAccessibleName(QStringLiteral("文件夹图标"));
    m_iconButton = glyph;
    nameLayout->addWidget(glyph);

    m_nameEdit = new QLineEdit(nameField);
    m_nameEdit->setObjectName(QStringLiteral("FolderNameEdit"));
    m_nameEdit->setPlaceholderText(QStringLiteral("名称"));
    m_nameEdit->setAccessibleName(QStringLiteral("文件夹名称"));
    m_nameEdit->setMaxLength(kListNameMaxLength);
    nameLayout->addWidget(m_nameEdit, 1);
    contentLayout()->addWidget(nameField);

    m_saveButton = new QPushButton(QStringLiteral("保存"), this);
    m_saveButton->setObjectName(QStringLiteral("FolderEditorSave"));
    m_saveButton->setProperty("primary", true);
    auto* cancel = new QPushButton(QStringLiteral("取消"), this);
    cancel->setObjectName(QStringLiteral("FolderEditorCancel"));
    buttonLayout()->addWidget(m_saveButton);
    buttonLayout()->addWidget(cancel);

    connect(glyph, &QToolButton::clicked, this, &FolderEditorDialog::openIconPicker);
    connect(m_nameEdit, &QLineEdit::textChanged, this, &FolderEditorDialog::revalidate);
    connect(m_nameEdit, &QLineEdit::returnPressed, this, [this]() {
        if (m_saveButton->isEnabled()) accept();
    });
    connect(m_saveButton, &QPushButton::clicked, this, &FolderEditorDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &FolderEditorDialog::reject);

    setNewFolderDefaults();
    applyTheme();
}

void FolderEditorDialog::setNewFolderDefaults()
{
    m_editing = false;
    m_original = Folder();
    m_icon.clear();
    static_cast<GlyphButton*>(m_iconButton)->setGlyph(QString());
    setDialogTitle(QStringLiteral("新建文件夹"));
    m_nameEdit->setMaxLength(kListNameMaxLength);
    m_nameEdit->setText(QStringLiteral("新文件夹"));
    m_nameEdit->selectAll();
    revalidate();
}

void FolderEditorDialog::setFolder(const Folder& folder)
{
    m_editing = true;
    m_original = folder;
    m_icon = folder.icon;
    static_cast<GlyphButton*>(m_iconButton)->setGlyph(m_icon);
    setDialogTitle(QStringLiteral("编辑文件夹"));
    m_nameEdit->setMaxLength(qMax<int>(kListNameMaxLength, folder.name.size()));
    m_nameEdit->setText(folder.name);
    m_nameEdit->selectAll();
    revalidate();
}

Folder FolderEditorDialog::draft() const
{
    Folder result = m_original;
    result.name = m_editing && m_nameEdit->text() == m_original.name
        ? m_original.name : m_nameEdit->text().trimmed();
    result.icon = m_icon;
    return result;
}

void FolderEditorDialog::openIconPicker()
{
    auto* picker = new EmojiPickerPopup(m_iconButton);
    picker->setAttribute(Qt::WA_DeleteOnClose);
    connect(picker, &EmojiPickerPopup::emojiSelected, this, [this](const QString& emoji) {
        m_icon = emoji;
        static_cast<GlyphButton*>(m_iconButton)->setGlyph(m_icon);
    });
    picker->popupAt(m_iconButton, true);
}

void FolderEditorDialog::revalidate()
{
    m_saveButton->setEnabled(!m_nameEdit->text().trimmed().isEmpty());
}

void FolderEditorDialog::applyTheme()
{
    ModalDialog::applyTheme();
    const auto color = [](ds::ColorToken token) {
        return ThemeManager::instance().color(token).name();
    };
    card()->setStyleSheet(card()->styleSheet() + QStringLiteral(
        "QFrame#FolderNameField{background:%1;border:1px solid %2;border-radius:8px;}"
        "QFrame#FolderNameField QLineEdit{background:transparent;border:0;padding:0;}"
        "QToolButton#FolderIconButton{border:0;padding:0;border-radius:8px;}")
        .arg(color(ds::ColorToken::Field), color(ds::ColorToken::Border)));
}

} // namespace kl
