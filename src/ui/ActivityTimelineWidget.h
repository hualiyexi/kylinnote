#ifndef KYLINLIST_ACTIVITYTIMELINEWIDGET_H
#define KYLINLIST_ACTIVITYTIMELINEWIDGET_H

// 任务动态时间线：任务详情正文下部的轻量 QWidget（滴答清单风格）。
//
// 定位与边界：
// - 无内部滚动，高度随事件条数自然生长，由宿主（TaskDetailForm 正文滚动区）承担滚动。
// - 组件自身横向 padding 0：滴答规格的外侧横向 20px 留白由宿主表单既有 24px 边距承担。
// - 关闭按钮只发 closeRequested()，由宿主决定隐藏方式。
// - 数据源：TaskStore::events(entityId) 惰性查询；TaskStore::eventsChanged(entityId)
//   只在 id 匹配且组件可见时触发 refresh，隐藏期间置脏、showEvent 时补一次。
// - 事件可见性统一收口在 kl::isVisibleActivityEvent(const EntityEvent&)（ActivityPolicy.h）。
// - 人名主体一律省略（个人本机应用，不读取机器账户、不伪造协作用户）。
// - 颜色全部来自 ds:: 语义 token，字体统一 ds::qfont()，不出现裸 QFont / 硬码颜色。
// - 摘要用纯文本 QLabel（不用富文本），用户内容不存在被解释为标记的风险；
//   时间线绝不显示 UUID、NULL、批次号，无 context 的旧日志降级为通用中文摘要。

#include <QFrame>
#include <QPointer>
#include <QWidget>
#include <QVector>
#include "../domain/Entity.h"

class QLabel;
class QToolButton;
class QVBoxLayout;

namespace kl {

class TaskStore;

class ActivityTimelineWidget : public QWidget {
    Q_OBJECT
public:
    explicit ActivityTimelineWidget(TaskStore* store, QWidget* parent = nullptr);

    void setStore(TaskStore* store);

    QString entityId() const { return m_entityId; }
    // 相同 id 直接返回（保持现有行，不重置状态）；切换 id 全量重建；空 id 清空所有行。
    void setEntityId(const QString& entityId);
    // 重新查询并重建事件行。
    void refresh();

signals:
    // 组件不隐藏自己，由宿主接收后隐藏。
    void closeRequested();

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    void onStoreEventsChanged(const QString& entityId);
    void applyTheme();

private:
    // 事件标记类别：圆底色 + 白色 glyph。
    enum class Marker { Create, Edit, Remove, Done, Restore };

    // 单条时间线行：左侧 1px 连接线 + 20px 圆形标记，右侧文字左缩 32px。
    // 布局自带行底 20px 间距（连接线画满整行高度以保持视觉连续），无 Q_OBJECT。
    class TimelineRow : public QFrame {
    public:
        TimelineRow(Marker marker, const QString& summaryText, const QString& timeText,
                    QWidget* parent);

        // first：首行，连接线不高于圆心；terminal：末行，连接线止于圆心。
        void setBounds(bool first, bool terminal);
        void applyTheme();

    protected:
        void paintEvent(QPaintEvent* event) override;

    private:
        Marker m_marker;
        bool m_first = false;
        bool m_terminal = false;
        QLabel* m_summary = nullptr;
        QLabel* m_time = nullptr;
    };

    void rebuild();
    void clearRows();
    // 生成中文可读摘要；marker 出参决定圆标类别。绝不返回 UUID/NULL/批次号。
    QString summaryFor(const EntityEvent& event, Marker* marker) const;

    QPointer<TaskStore> m_store;
    QString m_entityId;
    bool m_dirty = false;               // 隐藏期间错过 eventsChanged 的补偿标记
    QLabel* m_titleLabel = nullptr;
    QToolButton* m_closeButton = nullptr;
    QWidget* m_rowsContainer = nullptr;
    QVBoxLayout* m_rowsLayout = nullptr;
    QVector<TimelineRow*> m_rows;
};

} // namespace kl

#endif // KYLINLIST_ACTIVITYTIMELINEWIDGET_H
