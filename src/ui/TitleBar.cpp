#include "TitleBar.h"
#include "ColorTheme.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QToolButton>

namespace kl {

TitleBar::TitleBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("TitleBar"));
    setFixedHeight(TitleBarHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::ArrowCursor);
    setAttribute(Qt::WA_StyledBackground, true);

    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 3, 10, 0);
    layout->setSpacing(4);
    m_openButton = new QToolButton(this);
    m_openButton->setObjectName(QStringLiteral("OpenTaskButton"));
    m_openButton->setText(QStringLiteral("···"));
    m_openButton->setFixedSize(28, 24);
    m_openButton->setToolTip(QStringLiteral("打开待办详情，设置时间、提醒和附件"));
    layout->addWidget(m_openButton);
    connect(m_openButton, &QToolButton::clicked, this, &TitleBar::openClicked);
    layout->addStretch(1);

    m_colorButton = new QToolButton(this);
    m_colorButton->setObjectName(QStringLiteral("ColorButton"));
    m_colorButton->setFixedSize(20, 20);
    m_colorButton->setIconSize(QSize(14, 14));
    m_colorButton->setToolTip(QStringLiteral("\u5207\u6362\u989c\u8272\uff086 \u8272\u5faa\u73af\uff09"));
    m_colorButton->setCursor(Qt::ArrowCursor);
    layout->addWidget(m_colorButton);

    m_closeButton = new QToolButton(this);
    m_closeButton->setObjectName(QStringLiteral("CloseButton"));
    m_closeButton->setFixedSize(26, 26);
    m_closeButton->setText(QString::fromUtf8("\xC3\x97"));  // ×
    m_closeButton->setToolTip(QStringLiteral("取消贴到桌面（任务会保留在面板中）"));
    m_closeButton->setCursor(Qt::ArrowCursor);
    layout->addWidget(m_closeButton);

    connect(m_colorButton, &QToolButton::clicked, this, &TitleBar::colorClicked);
    connect(m_closeButton, &QToolButton::clicked, this, &TitleBar::closeClicked);
}

void TitleBar::setHovered(bool hovered) {
    m_openButton->setVisible(hovered);
    m_colorButton->setVisible(hovered);
}

void TitleBar::setColorIndicator(NoteColor color) {
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(0, 0, 0, 90), 1));
    painter.setBrush(QColor(ColorTheme::spec(color).body));
    painter.drawEllipse(1, 1, 12, 12);
    painter.end();

    m_colorButton->setIcon(QIcon(pixmap));
}

void TitleBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    QWidget* top = window();
    if (!top) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    m_dragStartGlobal = event->globalPos();
    m_dragStartWindowPos = top->frameGeometry().topLeft();
    event->accept();
}

void TitleBar::mouseMoveEvent(QMouseEvent* event) {
    if (!m_dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    QWidget* top = window();
    if (!top) {
        return;
    }
    const QPoint offset = event->globalPos() - m_dragStartGlobal;
    top->move(m_dragStartWindowPos + offset);
    event->accept();
}

void TitleBar::mouseReleaseEvent(QMouseEvent* event) {
    if (m_dragging && event->button() == Qt::LeftButton) {
        m_dragging = false;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

}  // namespace kl
