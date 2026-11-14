#include "ModalDialog.h"

#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QToolButton>
#include <QVBoxLayout>
#include "AppIcons.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {
namespace {

// 对话框阴影边距（透明窗口外圈留给投影的空间）。
constexpr int kShadowMargin = 24;

QString tokenColor(ds::ColorToken token)
{
    return ThemeManager::instance().color(token).name();
}

} // namespace

ModalDialog::ModalDialog(QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setObjectName(QStringLiteral("ModalDialog"));
    setAttribute(Qt::WA_TranslucentBackground);
    setFont(ds::qfont(ds::FontScene::Base));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kShadowMargin, kShadowMargin, kShadowMargin, kShadowMargin);
    outer->setSpacing(0);

    m_card = new QWidget(this);
    m_card->setObjectName(QStringLiteral("ModalCard"));
    m_card->setAttribute(Qt::WA_StyledBackground);
    auto* shadow = new QGraphicsDropShadowEffect(m_card);
    shadow->setBlurRadius(32);
    shadow->setOffset(0, 6);
    m_card->setGraphicsEffect(shadow);
    outer->addWidget(m_card);

    auto* cardLayout = new QVBoxLayout(m_card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    auto* header = new QWidget(m_card);
    header->setObjectName(QStringLiteral("ModalHeader"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(20, 12, 10, 10);
    headerLayout->setSpacing(8);
    m_titleLabel = new QLabel(header);
    m_titleLabel->setObjectName(QStringLiteral("ModalTitle"));
    m_titleLabel->setFont(ds::qfont(ds::FontScene::Section));
    headerLayout->addWidget(m_titleLabel);
    headerLayout->addStretch(1);
    m_closeButton = new QToolButton(header);
    m_closeButton->setObjectName(QStringLiteral("ModalClose"));
    m_closeButton->setFixedSize(28, 28);
    m_closeButton->setToolTip(QStringLiteral("关闭"));
    m_closeButton->setAccessibleName(QStringLiteral("关闭"));
    connect(m_closeButton, &QToolButton::clicked, this, &QDialog::reject);
    headerLayout->addWidget(m_closeButton);
    cardLayout->addWidget(header);

    m_contentLayout = new QVBoxLayout();
    m_contentLayout->setContentsMargins(20, 6, 20, 12);
    m_contentLayout->setSpacing(10);
    cardLayout->addLayout(m_contentLayout, 1);

    m_buttonLayout = new QHBoxLayout();
    m_buttonLayout->setContentsMargins(20, 0, 20, 16);
    m_buttonLayout->setSpacing(8);
    m_buttonLayout->addStretch(1); // 默认右对齐；子类直接 addWidget 追加按钮
    cardLayout->addLayout(m_buttonLayout);

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](ds::Theme) { applyTheme(); });
}

ModalDialog::~ModalDialog() = default;

void ModalDialog::setDialogTitle(const QString& title)
{
    m_titleLabel->setText(title);
    setWindowTitle(title);
}

QString ModalDialog::dialogTitle() const
{
    return m_titleLabel->text();
}

void ModalDialog::applyTheme()
{
    // 按钮族的完整局部样式：即使应用级 QSS 缺席（如独立测试环境）也能成型，
    // 同时补齐应用级没有的 danger 外观。danger 规则放在 primary 之后，
    // 同等特异性下后者胜出，保证危险态覆盖主按钮底色。
    m_card->setStyleSheet(QStringLiteral(
        "QWidget#ModalCard{background:%1;border:1px solid %2;border-radius:12px;}"
        "QWidget#ModalHeader{background:transparent;border:0;border-bottom:0px solid %3;}"
        "QLabel#ModalTitle{color:%4;background:transparent;border:0;}"
        "QLabel{background:transparent;border:0;}"
        "QToolButton#ModalClose{background:transparent;border:1px solid transparent;"
        "border-radius:6px;padding:0;}"
        "QToolButton#ModalClose:hover{background:%5;}"
        "QToolButton#ModalClose:pressed{background:%6;}"
        "QPushButton{color:%4;background:transparent;border:1px solid %2;"
        "border-radius:6px;padding:6px 14px;}"
        "QPushButton:hover{background:%5;}"
        "QPushButton:pressed{background:%6;}"
        "QPushButton:disabled{color:%7;border-color:%8;background:transparent;}"
        "QPushButton[primary=\"true\"]{background:%9;border:1px solid %9;color:%10;}"
        "QPushButton[primary=\"true\"]:hover{background:%11;border-color:%11;}"
        "QPushButton[primary=\"true\"]:pressed{background:%11;border-color:%11;}"
        "QPushButton[primary=\"true\"]:disabled{background:%8;border-color:%8;color:%7;}"
        "QPushButton[danger=\"true\"]{background:%12;border:1px solid %12;color:%10;}"
        "QPushButton[danger=\"true\"]:hover{background:%13;border-color:%13;}"
        "QPushButton[danger=\"true\"]:pressed{background:%13;border-color:%13;}"
        "QPushButton[danger=\"true\"]:disabled{background:%8;border-color:%8;color:%7;}"
        "QLabel#ModalError,QLabel#ListEditorError{color:%12;background:transparent;border:0;}")
        .arg(tokenColor(ds::ColorToken::Surface))
        .arg(tokenColor(ds::ColorToken::Border))
        .arg(tokenColor(ds::ColorToken::Divider))
        .arg(tokenColor(ds::ColorToken::Text))
        .arg(tokenColor(ds::ColorToken::Hover))
        .arg(tokenColor(ds::ColorToken::FieldHover))
        .arg(tokenColor(ds::ColorToken::MenuDisabled))
        .arg(tokenColor(ds::ColorToken::Field))
        .arg(tokenColor(ds::ColorToken::Primary))
        .arg(tokenColor(ds::ColorToken::OnPrimary))
        .arg(tokenColor(ds::ColorToken::PrimaryHover))
        .arg(tokenColor(ds::ColorToken::Danger))
        .arg(tokenColor(ds::ColorToken::Danger)));

    if (auto* effect = qobject_cast<QGraphicsDropShadowEffect*>(m_card->graphicsEffect())) {
        QColor tint = ThemeManager::instance().color(ds::ColorToken::Shadow);
        tint.setAlpha(46);
        effect->setColor(tint);
    }
    m_closeButton->setIcon(AppIcons::icon(IconId::Close));
    m_card->update();
}

void ModalDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    // 对齐父窗口所在屏幕：先算出最终尺寸，再在可用区域内居中并夹紧边界。
    layout()->activate();
    if (!testAttribute(Qt::WA_Resized)) adjustSize();
    QWidget* window = parentWidget() ? parentWidget()->window() : nullptr;
    QScreen* screen = nullptr;
    if (window) screen = QGuiApplication::screenAt(window->frameGeometry().center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    const QRect area = screen->availableGeometry();
    const QPoint center = window ? window->frameGeometry().center() : area.center();
    int x = center.x() - width() / 2;
    int y = center.y() - height() / 2;
    x = qBound(area.left(), x, qMax(area.left(), area.right() - width() + 1));
    y = qBound(area.top(), y, qMax(area.top(), area.bottom() - height() + 1));
    move(x, y);
}

ConfirmDialog::ConfirmDialog(const QString& title, const QString& text,
                             const QString& confirmText, bool danger, QWidget* parent)
    : ModalDialog(parent)
{
    setObjectName(QStringLiteral("ConfirmDialog"));
    setDialogTitle(title);
    setMinimumSize(448, 212);
    resize(448, 212);

    m_textLabel = new QLabel(this);
    m_textLabel->setObjectName(QStringLiteral("ConfirmText"));
    m_textLabel->setText(text);
    m_textLabel->setWordWrap(true);
    m_textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    contentLayout()->addWidget(m_textLabel);

    auto* cancelButton = new QPushButton(QStringLiteral("取消"), this);
    cancelButton->setObjectName(QStringLiteral("ConfirmCancel"));
    cancelButton->setAutoDefault(false);
    // 取消是默认按钮：Enter 直接落在取消上，危险操作宁可少做不做。
    cancelButton->setDefault(true);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

    auto* confirmButton = new QPushButton(confirmText, this);
    confirmButton->setObjectName(QStringLiteral("ConfirmAccept"));
    confirmButton->setProperty("primary", true);
    if (danger) confirmButton->setProperty("danger", true);
    connect(confirmButton, &QPushButton::clicked, this, &QDialog::accept);

    buttonLayout()->addWidget(cancelButton);
    buttonLayout()->addWidget(confirmButton);
    // 焦点与 Enter 都落在取消上：危险操作必须显式点击确认才执行。
    cancelButton->setFocus();
}

bool ConfirmDialog::prompt(const QString& title, const QString& text,
                           const QString& confirmText, bool danger, QWidget* parent)
{
    ConfirmDialog dialog(title, text, confirmText, danger, parent);
    return dialog.exec() == QDialog::Accepted;
}

} // namespace kl
