#include "NoteWidget.h"

#include "../app/NoteManager.h"
#include "../app/Settings.h"
#include "ColorTheme.h"
#include "NoteEditor.h"
#include "ResizeGrip.h"
#include "TitleBar.h"

#include <QEvent>
#include <QLabel>
#include <QMoveEvent>
#include <QTextCursor>
#include <QTimer>
#include <QDebug>
#include <QVBoxLayout>

namespace kn {

NoteWidget::NoteWidget(NoteManager *manager, const NoteRecord &record, QWidget *parent)
    : QWidget(parent),
      m_manager(manager),
      m_record(record)
{
    setObjectName(QStringLiteral("NoteWidget"));
    // 自定义 QWidget 需要显式开启，样式表的 background 才会被绘制
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMinimumSize(MinWidth, MinHeight);
    setFocusPolicy(Qt::StrongFocus);

    buildUi();

    m_editor->setHtml(m_record.content);
    setColor(m_record.color);

    if (m_record.geometry.isValid() && m_record.geometry.width() >= MinWidth
        && m_record.geometry.height() >= MinHeight) {
        setGeometry(m_record.geometry);
    } else {
        resize(DefaultWidth, DefaultHeight);
    }

    applyWindowFlags();

    connect(m_editor, &QTextEdit::textChanged, this, &NoteWidget::onTextChanged);
    connect(m_titleBar, &TitleBar::closeClicked, this, [this]() {
        emit closeRequested(this);
    });
    connect(m_titleBar, &TitleBar::colorClicked, this, [this]() {
        cycleColor();
    });

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(SaveDebounceMs);
    connect(&m_saveTimer, &QTimer::timeout, this, &NoteWidget::onSaveTimeout);
}

void NoteWidget::buildUi()
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_titleBar = new TitleBar(this);
    layout->addWidget(m_titleBar);

    m_editor = new NoteEditor(this);
    layout->addWidget(m_editor, 1);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("StatusLabel"));
    m_status->setText(QString::fromUtf8("\u25CF \u672a\u4fdd\u5b58\uff1a\u5199\u5165\u6570\u636e\u5e93\u5931\u8d25\uff0c\u5c06\u81ea\u52a8\u91cd\u8bd5"));
    m_status->hide();
    layout->addWidget(m_status);

    m_grip = new ResizeGrip(this);
    m_grip->raise();

    m_titleBar->installEventFilter(this);
    m_editor->installEventFilter(this);
}

NoteRecord NoteWidget::snapshot() const
{
    NoteRecord rec = m_record;
    rec.content  = m_editor->toHtml();
    rec.title    = deriveTitle(rec.content);
    rec.geometry = geometry();
    return rec;
}

void NoteWidget::applySaved(const NoteRecord &saved)
{
    m_record = saved;
    m_record.geometry = geometry();
}

void NoteWidget::setColor(NoteColor color)
{
    m_record.color = color;
    ColorTheme::apply(this, color);
    m_titleBar->setColorIndicator(color);
}

void NoteWidget::cycleColor()
{
    setColor(ColorTheme::next(m_record.color));
    // 颜色变更立即落库，不等待防抖
    scheduleSave();
}

NoteLayer NoteWidget::cycleLayer()
{
    setLayer(nextLayer(m_layer));
    return m_layer;
}

void NoteWidget::setLayer(NoteLayer layer)
{
    if (m_layer == layer) {
        return;
    }
    m_layer = layer;
    applyWindowFlags();
    if (isVisible()) {
        raise();
    }
}

void NoteWidget::raiseTemporarily()
{
    m_tempRaised = true;
    m_layer      = NoteLayer::Normal;   // 先浮起来让用户看到
    applyWindowFlags();
    show();
    raise();
    activateWindow();
    focusEditor();
}

void NoteWidget::applyWindowFlags()
{
    m_applyingFlags = true;
    Qt::WindowFlags flags = Qt::Tool | Qt::FramelessWindowHint;
    switch (m_layer) {
    case NoteLayer::OnTop:
        flags |= Qt::WindowStaysOnTopHint;
        break;
    case NoteLayer::OnDesktop:
        flags |= Qt::WindowStaysOnBottomHint;
        break;
    case NoteLayer::Normal:
        break;   // 不加 hint，与其它应用窗口按焦点正常竞争
    }

    const bool wasVisible = isVisible();
    const QRect geo = geometry();

    setWindowFlags(flags);   // 会隐式隐藏窗口，需要恢复几何与可见性
    setGeometry(geo);
    setMinimumSize(MinWidth, MinHeight);

    if (wasVisible) {
        show();
    }
    m_applyingFlags = false;
}

void NoteWidget::setSaveFailed(bool failed)
{
    m_saveFailed = failed;
    m_status->setVisible(failed);
    if (failed) {
        m_grip->raise();
    }
}

void NoteWidget::layoutGrip()
{
    if (!m_grip) {
        return;
    }
    m_grip->move(width() - m_grip->width(), height() - m_grip->height());
    m_grip->raise();
}

void NoteWidget::raiseToTop()
{
    show();
    raise();
}

void NoteWidget::delayedRaise(int delayMs)
{
    // 窗管可能晚于进程设置 _NET_WM_STATE_ABOVE，延迟补一次置顶
    QTimer::singleShot(delayMs, this, [this]() {
        if (isVisible()) {
            raise();
        }
    });
}

void NoteWidget::focusEditor()
{
    show();
    raise();
    activateWindow();
    if (m_editor) {
        m_editor->setFocus();
        QTextCursor cursor = m_editor->textCursor();
        cursor.movePosition(QTextCursor::End);
        m_editor->setTextCursor(cursor);
    }
}

void NoteWidget::updateGeometryRecord()
{
    m_record.geometry = geometry();
}

void NoteWidget::scheduleSave()
{
    m_saveTimer.start();   // 已运行时等同重启计时，实现防抖
}

void NoteWidget::onTextChanged()
{
    scheduleSave();
}

void NoteWidget::onSaveTimeout()
{
    emit saveRequested(this);
}

void NoteWidget::moveEvent(QMoveEvent *event)
{
    updateGeometryRecord();
    scheduleSave();
    QWidget::moveEvent(event);
}

void NoteWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateGeometryRecord();
    layoutGrip();
    scheduleSave();
}

void NoteWidget::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        emit activated(this);
    }
    // 新建便签临时置前后，一旦失焦就沉回用户设定的默认层级
    if (event->type() == QEvent::WindowDeactivate && m_tempRaised
        && !m_applyingFlags) {
        m_tempRaised = false;
        m_layer      = Settings::instance().defaultLayer();
        applyWindowFlags();
    }
    QWidget::changeEvent(event);
}

bool NoteWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_editor || watched == m_titleBar) {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::FocusIn:
        case QEvent::KeyPress:
            emit activated(this);
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace kn
