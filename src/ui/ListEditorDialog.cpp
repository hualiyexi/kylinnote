#include "ListEditorDialog.h"

#include <QComboBox>
#include <QConicalGradient>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include "AppIcons.h"
#include "ColorPickerPopup.h"
#include "EmojiPickerPopup.h"
#include "GlyphButton.h"
#include "ListAppearance.h"
#include "MenuComboBox.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {
namespace {

// 7 个清单预设色：用户可选数据，非主题色，允许固定字面量。
const char* const kListPresetColors[] = {
    "#FF6161", "#FFAC38", "#FFD324", "#E6EA49", "#35D870", "#4CA1FF", "#6E75F4",
};

constexpr int kLabelWidth = 72;
constexpr int kSwatchSize = 22;

QColor token(ds::ColorToken t) { return ThemeManager::instance().color(t); }

} // namespace

// ---------------------------------------------------------------------------
// 圆形颜色块：Solid 预设色 / NoneMark 无颜色（斜杠）/ Rainbow 自定义入口。
// 选中环与 hover 底色取主题令牌；圆内色值为固定数据。
// ---------------------------------------------------------------------------
class ListSwatch : public QToolButton {
public:
    enum class Kind { Solid, NoneMark, Rainbow };

    ListSwatch(Kind kind, const QColor& fixedColor, QWidget* parent)
        : QToolButton(parent), m_kind(kind), m_color(fixedColor)
    {
        setCheckable(true);
        setFixedSize(kSwatchSize, kSwatchSize);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
    }

    // 仅 Rainbow：设置最近一次的自定义颜色；有效时画实心圆。
    void setCustomColor(const QColor& color)
    {
        m_custom = color;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (underMouse() || hasFocus()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(token(ds::ColorToken::Hover));
            painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 6, 6);
        }
        const QRectF circle = QRectF(rect()).adjusted(3.5, 3.5, -3.5, -3.5);
        if (m_kind == Kind::Solid) {
            painter.setPen(QPen(token(ds::ColorToken::Border), 1.0));
            painter.setBrush(m_color);
            painter.drawEllipse(circle);
        } else if (m_kind == Kind::NoneMark) {
            // 无颜色：空心圆 + 斜杠。
            painter.setPen(QPen(token(ds::ColorToken::Border), 1.0));
            painter.setBrush(token(ds::ColorToken::Field));
            painter.drawEllipse(circle);
            painter.setPen(QPen(token(ds::ColorToken::TextSecondary), 1.6));
            const QPointF center = circle.center();
            painter.drawLine(center + QPointF(-3.5, 3.5), center + QPointF(3.5, -3.5));
        } else if (m_custom.isValid()) {
            painter.setPen(QPen(token(ds::ColorToken::Border), 1.0));
            painter.setBrush(m_custom);
            painter.drawEllipse(circle);
        } else {
            // 彩虹环：自定义颜色入口。
            const QPointF center = circle.center();
            const qreal outer = circle.width() / 2.0;
            const qreal inner = outer - 4.0;
            QConicalGradient gradient(center, 90);
            gradient.setColorAt(0.00, QColor(0xFF, 0x00, 0x00));
            gradient.setColorAt(0.17, QColor(0xFF, 0xD3, 0x00));
            gradient.setColorAt(0.33, QColor(0x00, 0xE5, 0x33));
            gradient.setColorAt(0.50, QColor(0x00, 0xD8, 0xFF));
            gradient.setColorAt(0.67, QColor(0x35, 0x5C, 0xFF));
            gradient.setColorAt(0.83, QColor(0xC7, 0x2A, 0xFF));
            gradient.setColorAt(1.00, QColor(0xFF, 0x00, 0x00));
            QPainterPath ring;
            ring.addEllipse(center, outer, outer);
            ring.addEllipse(center, inner, inner);
            painter.fillPath(ring, gradient);
            painter.setPen(QPen(token(ds::ColorToken::Border), 1.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(center, outer, outer);
        }
        if (isChecked()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(token(ds::ColorToken::Surface), 2.0));
            painter.drawEllipse(circle.adjusted(-1.0, -1.0, 1.0, 1.0));
            painter.setPen(QPen(token(ds::ColorToken::Primary), 2.0));
            painter.drawEllipse(circle.adjusted(-2.5, -2.5, 2.5, 2.5));
        }
    }

private:
    Kind m_kind;
    QColor m_color;
    QColor m_custom;
};

class NameFieldFrame : public QWidget {
public:
    explicit NameFieldFrame(QWidget* parent) : QWidget(parent) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(token(ds::ColorToken::Field));
        const auto border = property("focused").toBool() ? ds::ColorToken::FocusRing
                                                        : ds::ColorToken::Border;
        painter.setPen(QPen(token(border), 1.0));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    }
};

// ---------------------------------------------------------------------------
ListEditorDialog::ListEditorDialog(const QVector<TaskList>& lists,
                                   const QVector<Folder>& folders, QWidget* parent)
    : ModalDialog(parent), m_lists(lists), m_folders(folders)
{
    setObjectName(QStringLiteral("ListEditorDialog"));
    // 文件夹按显示顺序稳定排序，避免依赖调用方传入顺序。
    std::sort(m_folders.begin(), m_folders.end(), [](const Folder& a, const Folder& b) {
        if (a.sortOrder != b.sortOrder) return a.sortOrder < b.sortOrder;
        return a.name.localeAwareCompare(b.name) < 0;
    });
    setDialogTitle(QStringLiteral("添加清单"));
    setFixedWidth(480);
    buildUi();
    setNewListDefaults(QString(), EntityKind::Task); // 新建模式默认值
    applyTheme(); // 基类构造时虚函数尚未派发到本类，这里补上表单局部样式
    adjustSize();
}

void ListEditorDialog::buildUi()
{
    auto addFieldLabel = [this](const QString& text) {
        auto* label = new QLabel(text, this);
        label->setObjectName(QStringLiteral("ListFieldLabel"));
        label->setFixedWidth(kLabelWidth);
        return label;
    };

    // 名称行：与滴答一致，图标、分隔线和“名称”占位都在输入框内。
    auto* nameRow = new QHBoxLayout();
    nameRow->setSpacing(0);
    auto* nameField = new NameFieldFrame(this);
    m_nameField = nameField;
    nameField->setObjectName(QStringLiteral("ListNameField"));
    nameField->setFixedHeight(36);
    auto* nameLayout = new QHBoxLayout(nameField);
    nameLayout->setContentsMargins(2, 2, 8, 2);
    nameLayout->setSpacing(4);
    m_iconButton = new GlyphButton(nameField);
    m_iconButton->setObjectName(QStringLiteral("ListIconButton"));
    m_iconButton->setFixedSize(30, 30);
    m_iconButton->setIconSize(QSize(18, 18));
    m_iconButton->setToolTip(QStringLiteral("选择清单图标"));
    m_iconButton->setAccessibleName(QStringLiteral("清单图标"));
    m_iconButton->setCursor(Qt::PointingHandCursor);
    connect(m_iconButton, &QToolButton::clicked, this, &ListEditorDialog::openIconPicker);
    auto* separator = new QWidget(nameField);
    separator->setObjectName(QStringLiteral("ListNameSeparator"));
    separator->setFixedSize(1, 20);
    m_nameEdit = new QLineEdit(nameField);
    m_nameEdit->setObjectName(QStringLiteral("ListNameEdit"));
    m_nameEdit->setMaxLength(kListNameMaxLength);
    m_nameEdit->setPlaceholderText(QStringLiteral("名称"));
    m_nameEdit->setAccessibleName(QStringLiteral("清单名称"));
    m_nameEdit->installEventFilter(this);
    m_iconButton->installEventFilter(this);
    connect(m_nameEdit, &QLineEdit::textChanged, this, [this]() { revalidate(); });
    connect(m_nameEdit, &QLineEdit::returnPressed, this, &ListEditorDialog::tryAccept);
    nameLayout->addWidget(m_iconButton);
    nameLayout->addWidget(separator);
    nameLayout->addWidget(m_nameEdit, 1);
    nameRow->addWidget(nameField, 1);
    contentLayout()->addLayout(nameRow);

    m_errorLabel = new QLabel(this);
    m_errorLabel->setObjectName(QStringLiteral("ListEditorError"));
    m_errorLabel->hide();
    auto* errorRow = new QHBoxLayout();
    errorRow->setContentsMargins(4, 0, 0, 0);
    errorRow->addWidget(m_errorLabel);
    contentLayout()->addLayout(errorRow);

    // 颜色行：无颜色 + 7 预设 + 自定义入口。
    auto* colorRow = new QHBoxLayout();
    colorRow->setSpacing(4);
    colorRow->addWidget(addFieldLabel(QStringLiteral("清单颜色")));
    m_noneColorButton = new ListSwatch(ListSwatch::Kind::NoneMark, QColor(), this);
    m_noneColorButton->setObjectName(QStringLiteral("ListColorNoneButton"));
    m_noneColorButton->setToolTip(QStringLiteral("无颜色"));
    m_noneColorButton->setAccessibleName(QStringLiteral("无颜色"));
    m_noneColorButton->setCheckable(true);
    m_noneColorButton->setFixedSize(kSwatchSize, kSwatchSize);
    m_noneColorButton->setFocusPolicy(Qt::StrongFocus);
    m_noneColorButton->setCursor(Qt::PointingHandCursor);
    connect(m_noneColorButton, &QToolButton::clicked, this,
            [this]() { applyColorSelection(QColor()); });
    colorRow->addWidget(m_noneColorButton);
    for (const char* hex : kListPresetColors) {
        const QColor color(QString::fromLatin1(hex));
        auto* swatch = new ListSwatch(ListSwatch::Kind::Solid, color, this);
        swatch->setObjectName(QStringLiteral("ListColorButton"));
        swatch->setProperty("hex", color.name());
        swatch->setToolTip(color.name().toUpper());
        swatch->setAccessibleName(QStringLiteral("清单颜色 %1").arg(color.name().toUpper()));
        connect(swatch, &QToolButton::clicked, this,
                [this, color]() { applyColorSelection(color); });
        m_presetColorButtons.append(swatch);
        colorRow->addWidget(swatch);
    }
    m_customColorButton = new ListSwatch(ListSwatch::Kind::Rainbow, QColor(), this);
    m_customColorButton->setObjectName(QStringLiteral("ListCustomColorButton"));
    m_customColorButton->setToolTip(QStringLiteral("自定义颜色"));
    m_customColorButton->setAccessibleName(QStringLiteral("自定义颜色"));
    connect(m_customColorButton, &QToolButton::clicked, this,
            &ListEditorDialog::openColorPicker);
    colorRow->addWidget(m_customColorButton);
    colorRow->addStretch(1);
    contentLayout()->addLayout(colorRow);

    // 文件夹行：无 + 真实 folderId。
    auto* folderRow = new QHBoxLayout();
    folderRow->setSpacing(8);
    folderRow->addWidget(addFieldLabel(QStringLiteral("文件夹")));
    m_folderCombo = new MenuComboBox(this);
    m_folderCombo->setObjectName(QStringLiteral("ListFolderCombo"));
    m_folderCombo->setAccessibleName(QStringLiteral("所属文件夹"));
    m_folderCombo->addItem(QStringLiteral("无"), QString());
    for (const Folder& folder : m_folders) {
        m_folderCombo->addItem(folder.name, folder.id);
        m_folderCombo->setItemData(m_folderCombo->count() - 1, folder.name, Qt::ToolTipRole);
    }
    connect(m_folderCombo,
            static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
            [this](int) { revalidate(); });
    folderRow->addWidget(m_folderCombo, 1);
    contentLayout()->addLayout(folderRow);

    // 底部按钮：取消 + 添加/保存。
    m_cancelButton = new QPushButton(QStringLiteral("取消"), this);
    m_cancelButton->setObjectName(QStringLiteral("ListEditorCancel"));
    m_cancelButton->setAutoDefault(false);
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    m_saveButton = new QPushButton(QStringLiteral("添加"), this);
    m_saveButton->setObjectName(QStringLiteral("ListEditorSave"));
    m_saveButton->setProperty("primary", true);
    m_saveButton->setAutoDefault(false);
    connect(m_saveButton, &QPushButton::clicked, this, &ListEditorDialog::tryAccept);
    buttonLayout()->addWidget(m_cancelButton);
    buttonLayout()->addWidget(m_saveButton);

    m_nameEdit->setFocus();
}

void ListEditorDialog::setNewListDefaults(const QString& folderId, EntityKind kind)
{
    m_editing = false;
    m_newKind = kind;
    m_original = TaskList();
    setDialogTitle(QStringLiteral("添加清单"));
    m_saveButton->setText(QStringLiteral("添加"));
    m_nameEdit->setMaxLength(kListNameMaxLength);
    m_nameEdit->setText(QString());
    m_icon = QString();
    updateIconButton();
    m_customColor = QColor();
    m_customColorButton->setCustomColor(QColor());
    applyColorSelection(QColor());
    int folderIndex = 0;
    for (int i = 0; i < m_folderCombo->count(); ++i) {
        if (m_folderCombo->itemData(i).toString() == folderId) {
            folderIndex = i;
            break;
        }
    }
    m_folderCombo->setCurrentIndex(folderIndex);
    revalidate();
    m_nameEdit->setFocus();
    m_nameEdit->selectAll();
}

void ListEditorDialog::setList(const TaskList& existing)
{
    if (existing.id.isEmpty()) {
        // 空清单等价于新建默认值（兼容旧用法）。
        setNewListDefaults(existing.folderId, existing.kind);
        return;
    }
    m_editing = true;
    m_newKind = existing.kind;
    m_original = existing;
    setDialogTitle(QStringLiteral("编辑清单"));
    m_saveButton->setText(QStringLiteral("保存"));
    m_nameEdit->setMaxLength(qMax<int>(kListNameMaxLength, existing.name.size()));
    m_nameEdit->setText(existing.name);
    m_icon = existing.icon;
    updateIconButton();
    m_customColor = QColor();
    const QColor color(existing.color);
    if (color.isValid()) {
        // 预设之外的颜色按“自定义入口最近颜色”回填，保证自定义色块可视。
        bool isPreset = false;
        for (const char* hex : kListPresetColors) {
            if (color.name().compare(QString::fromLatin1(hex), Qt::CaseInsensitive) == 0)
                isPreset = true;
        }
        if (!isPreset) m_customColor = color;
        applyColorSelection(color);
    } else {
        applyColorSelection(QColor());
    }
    int folderIndex = 0;
    for (int i = 0; i < m_folderCombo->count(); ++i) {
        if (m_folderCombo->itemData(i).toString() == existing.folderId) {
            folderIndex = i;
            break;
        }
    }
    m_folderCombo->setCurrentIndex(folderIndex);
    m_customColorButton->setCustomColor(m_customColor);
    revalidate();
    m_nameEdit->setFocus();
    m_nameEdit->selectAll();
}

void ListEditorDialog::updateIconButton()
{
    if (auto* button = static_cast<GlyphButton*>(m_iconButton)) {
        button->setGlyph(m_icon);
        button->setDefaultIconName(m_newKind == EntityKind::Note
            ? QStringLiteral("note-list") : QStringLiteral("normal-list"));
    }
    m_iconButton->setAccessibleName(m_icon.isEmpty()
                                        ? QStringLiteral("清单图标（未设置）")
                                        : QStringLiteral("清单图标 %1").arg(m_icon));
}

void ListEditorDialog::applyColorSelection(const QColor& color)
{
    m_color = color;
    m_noneColorButton->setChecked(!color.isValid());
    for (ListSwatch* swatch : m_presetColorButtons)
        swatch->setChecked(color.isValid() && swatch->property("hex").toString()
                                                   .compare(color.name(), Qt::CaseInsensitive) == 0);
    m_customColorButton->setChecked(m_customColor.isValid() && color == m_customColor);
}

void ListEditorDialog::openIconPicker()
{
    auto* picker = new EmojiPickerPopup(m_iconButton);
    picker->setAttribute(Qt::WA_DeleteOnClose);
    connect(picker, &EmojiPickerPopup::emojiSelected, this, [this](const QString& emoji) {
        m_icon = emoji; // 空串 = 重置为默认图标
        updateIconButton();
    });
    picker->popupAt(m_iconButton, true);
}

void ListEditorDialog::openColorPicker()
{
    auto* picker = new ColorPickerPopup(m_customColorButton);
    picker->setAttribute(Qt::WA_DeleteOnClose);
    QColor initial = m_color.isValid() ? m_color : m_customColor;
    if (initial.isValid()) picker->setInitialColor(initial);
    connect(picker, &ColorPickerPopup::colorSelected, this, [this](const QColor& color) {
        m_customColor = color;
        m_customColorButton->setCustomColor(color);
        applyColorSelection(color);
    });
    picker->popupAt(m_customColorButton, true);
}

void ListEditorDialog::revalidate()
{
    const QString name = draftName();
    QString error;
    const bool unchangedName = m_editing && name == m_original.name;
    if (!unchangedName && name.size() > kListNameMaxLength) {
        error = QStringLiteral("名称最多 %1 个字符").arg(kListNameMaxLength);
    } else if (!name.isEmpty()) {
        const QString folderId = draftFolderId();
        for (const TaskList& list : m_lists) {
            if (m_editing && list.id == m_original.id) continue;
            if (list.archivedAtUtcMs > 0) continue; // 已归档清单不占名
            if (list.folderId == folderId
                && QString::compare(list.name.trimmed(), name, Qt::CaseInsensitive) == 0) {
                error = folderId.isEmpty() ? QStringLiteral("已存在同名清单")
                                           : QStringLiteral("该文件夹下已存在同名清单");
                break;
            }
        }
    }
    m_errorLabel->setText(error);
    m_errorLabel->setVisible(!error.isEmpty());
    m_saveButton->setEnabled(!name.isEmpty() && error.isEmpty());
}

void ListEditorDialog::tryAccept()
{
    if (!m_saveButton->isEnabled()) return;
    accept();
}

TaskList ListEditorDialog::draft() const
{
    TaskList list = m_editing ? m_original : TaskList();
    if (!m_editing) list.kind = m_newKind;
    list.name = draftName();
    list.folderId = draftFolderId();
    list.color = draftColorHex();
    list.icon = draftIcon();
    return list;
}

QString ListEditorDialog::draftName() const
{
    const QString text = m_nameEdit->text();
    return m_editing && text == m_original.name ? m_original.name : text.trimmed();
}

QString ListEditorDialog::draftColorHex() const
{
    return m_color.isValid() ? m_color.name() : QString();
}

QString ListEditorDialog::draftIcon() const
{
    return m_icon;
}

QString ListEditorDialog::draftFolderId() const
{
    return m_folderCombo->currentData().toString();
}

bool ListEditorDialog::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == m_nameEdit || watched == m_iconButton)
        && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
        QTimer::singleShot(0, this, [this]() {
            m_nameField->setProperty("focused", m_nameEdit->hasFocus() || m_iconButton->hasFocus());
            m_nameField->update();
        });
    }
    return ModalDialog::eventFilter(watched, event);
}

void ListEditorDialog::applyTheme()
{
    ModalDialog::applyTheme();
    card()->setStyleSheet(card()->styleSheet() + QStringLiteral(
        "QLabel#ListFieldLabel{color:%1;background:transparent;border:0;}"
        "QWidget#ListNameSeparator{background:%4;border:0;}"
        "QToolButton#ListIconButton{background:transparent;border:1px solid transparent;"
        "border-radius:6px;padding:0;}"
        "QToolButton#ListIconButton:hover{background:%5;}"
        "QToolButton#ListIconButton:focus{border-color:%3;}"
        "QComboBox#ListFolderCombo{background:%2;border:1px solid transparent;border-radius:6px;"
        "padding:5px 8px;color:%1;}"
        "QComboBox#ListFolderCombo:hover{background:%6;}"
        "QComboBox#ListFolderCombo:focus{border-color:%3;background:%2;}"
        "QComboBox#ListFolderCombo::drop-down{border:0;width:22px;}"
        "QComboBox QAbstractItemView{background:%7;border:1px solid %8;border-radius:6px;"
        "padding:4px;selection-background-color:%9;selection-color:%1;outline:0;}")
        .arg(token(ds::ColorToken::Text).name())
        .arg(token(ds::ColorToken::Field).name())
        .arg(token(ds::ColorToken::FocusRing).name())
        .arg(token(ds::ColorToken::Divider).name())
        .arg(token(ds::ColorToken::Hover).name())
        .arg(token(ds::ColorToken::FieldHover).name())
        .arg(token(ds::ColorToken::MenuBg).name())
        .arg(token(ds::ColorToken::Border).name())
        .arg(token(ds::ColorToken::Selection).name()));
    m_nameEdit->setStyleSheet(QStringLiteral(
        "QLineEdit,QLineEdit:focus,QLineEdit:hover{background:transparent;border:0;"
        "border-radius:0;padding:4px 2px;color:%1;selection-background-color:%2;selection-color:%3;}")
        .arg(token(ds::ColorToken::Text).name())
        .arg(token(ds::ColorToken::Primary).name())
        .arg(token(ds::ColorToken::OnPrimary).name()));
    m_nameField->update();
    updateIconButton();
}

} // namespace kl
