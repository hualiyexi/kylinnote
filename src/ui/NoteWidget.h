#ifndef KYLINNOTE_NOTEWIDGET_H
#define KYLINNOTE_NOTEWIDGET_H

#include "../Common.h"

#include <QTimer>
#include <QWidget>

class QLabel;
class QVBoxLayout;
class QMoveEvent;
class QResizeEvent;

namespace kn {

class NoteManager;
class TitleBar;
class NoteEditor;
class ResizeGrip;

class NoteWidget : public QWidget
{
    Q_OBJECT
public:
    NoteWidget(NoteManager *manager, const NoteRecord &record, QWidget *parent = nullptr);

    qint64 id() const { return m_record.id; }
    const NoteRecord &record() const { return m_record; }
    int zOrder() const { return m_record.zOrder; }

    // 从界面收集当前状态（内容、几何、颜色、层级）
    NoteRecord snapshot() const;
    // 写库成功后回写自增 id 等字段
    void applySaved(const NoteRecord &saved);

    void setZOrder(int z) { m_record.zOrder = z; }
    NoteColor color() const { return m_record.color; }
    void setColor(NoteColor color);
    void cycleColor();

    // 在「贴在桌面 / 普通 / 置顶」之间循环，返回切换后的层级
    NoteLayer cycleLayer();
    void setLayer(NoteLayer layer);
    NoteLayer layer() const { return m_layer; }
    bool alwaysOnTop() const { return m_layer == NoteLayer::OnTop; }

    void setSaveFailed(bool failed);
    bool saveFailed() const { return m_saveFailed; }

    void markDeleted(bool deleted) { m_markedDeleted = deleted; }
    bool isMarkedDeleted() const { return m_markedDeleted; }

    void raiseToTop();
    void delayedRaise(int delayMs = 800);
    void focusEditor();

    // 新建便签时临时置前，保证"点了就有反应"；失去焦点后回到默认层级。
    // 贴桌面模式下便签位于所有应用窗口之下，不这样做新建的便签会被挡住看不见。
    void raiseTemporarily();

signals:
    void activated(NoteWidget *note);
    void closeRequested(NoteWidget *note);
    void saveRequested(NoteWidget *note);

protected:
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onTextChanged();
    void onSaveTimeout();

private:
    void buildUi();
    void updateGeometryRecord();
    void scheduleSave();
    void applyWindowFlags();
    void layoutGrip();

    NoteManager *m_manager = nullptr;
    NoteRecord  m_record;

    TitleBar    *m_titleBar = nullptr;
    NoteEditor  *m_editor   = nullptr;
    QLabel      *m_status   = nullptr;
    ResizeGrip  *m_grip     = nullptr;

    QTimer    m_saveTimer;
    NoteLayer m_layer      = NoteLayer::OnDesktop;
    bool      m_saveFailed    = false;
    bool      m_markedDeleted = false;
    bool      m_tempRaised    = false;
    bool      m_applyingFlags = false;
};

} // namespace kn

#endif // KYLINNOTE_NOTEWIDGET_H
