#pragma once

#include <QList>
#include <QPointer>
#include <QWidget>

namespace kl {
class TaskDetailWidget;
class TaskStore;
class WindowResizeArea;

// 独立顶级关联详情窗口；内容与第四列共用，不重挂源详情控件。
class RelatedDetailPopup final : public QWidget {
    Q_OBJECT
public:
    explicit RelatedDetailPopup(TaskStore* store);
    ~RelatedDetailPopup() override;
    bool openEntity(const QString& id);
    QString entityId() const;
    bool finishEditing();
    bool requestClose();
    // 恢复前已由应用保存并结束会话；成功恢复后不能再提交旧编辑上下文。
    void clearAfterRestore();
    void setAnchorBounds(const QRect& globalBounds) { m_anchorBounds = globalBounds; }

signals:
    void internalLinkRequested(QString entityId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool focusNextPrevChild(bool next) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, long* result) override;

private:
    void createDetail();
    void clearDetail(bool restoreFocus);
    void positionInitially();
    void syncWindowDecoration();
    void applyTheme();
    QPointer<TaskStore> m_store;
    QPointer<TaskDetailWidget> m_detail;
    QPointer<QWidget> m_previousFocus;
    QWidget* m_card = nullptr;
    QList<WindowResizeArea*> m_resizeAreas;
    QRect m_anchorBounds;
    bool m_positioned = false;
    bool m_closing = false;
    quint64 m_generation = 0;
};
}  // namespace kl
