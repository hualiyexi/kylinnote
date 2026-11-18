#include "ActivityTimelineWidget.h"

#include <QDate>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QShowEvent>
#include <QToolButton>
#include <QVBoxLayout>
#include "../Common.h"
#include "../app/TaskStore.h"
#include "../domain/ActivityPolicy.h"
#include "AppIcons.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {
namespace {

// 滴答清单计算值：20px 圆形标记、文字左缩 32px、1px 连接线、
// 摘要 14px、时间 12px/20px、条目间 20px、标题上 18 下 8、close 18。
constexpr int MarkerSize = 20;
constexpr int MarkerGlyphSize = 18;
constexpr int TextIndent = 32;
constexpr int RowBottomGap = 20;
constexpr int SummaryTimeGap = 4;
constexpr int TimeLineHeight = 20;
constexpr int HeaderTop = 18;
constexpr int HeaderBottom = 8;
constexpr int CloseSize = 18;
constexpr int PreviewMaxChars = 16;

QString kindLabel(EntityKind kind)
{
    return kind == EntityKind::Note ? QStringLiteral("笔记") : QStringLiteral("任务");
}

QString variantText(const QVariant& value)
{
    return value.isValid() ? value.toString() : QString();
}

// 短预览：折叠空白、按码点计数截断，绝不把整段长文塞进时间线。
QString elidedPreview(const QString& text, int maxChars)
{
    const QString flat = text.simplified();
    int count = 0;
    for (const QChar unit : flat) {
        if (!unit.isHighSurrogate()) ++count;   // 代理对按一个码点计
    }
    if (count <= maxChars) return flat;
    int kept = 0;
    int index = 0;
    while (index < flat.size() && kept < maxChars) {
        if (!flat.at(index).isHighSurrogate()) ++kept;
        ++index;
    }
    return flat.left(index).trimmed() + QStringLiteral("…");
}

QString statusLabel(const QString& name)
{
    switch (taskStatusFromName(name)) {
    case TaskStatus::Completed: return QStringLiteral("已完成");
    case TaskStatus::Abandoned: return QStringLiteral("已放弃");
    case TaskStatus::Active: break;
    }
    return QStringLiteral("进行中");
}

QString priorityLabel(const QString& text)
{
    switch (text.toInt()) {
    case 1: return QStringLiteral("低");
    case 2: return QStringLiteral("中");
    case 3: return QStringLiteral("高");
    case 0: break;
    }
    return QStringLiteral("无");
}

QString recurrenceLabel(const QString& rule)
{
    if (rule.isEmpty() || rule == QLatin1String("none")) return QStringLiteral("不重复");
    QString frequency;
    int interval = 1;
    if (!parseRecurrence(rule, &frequency, &interval) || frequency.isEmpty())
        return rule;
    const QString unit = frequency == QStringLiteral("DAILY") ? QStringLiteral("天")
        : frequency == QStringLiteral("WEEKLY") ? QStringLiteral("周")
        : QStringLiteral("月");
    return interval <= 1 ? QStringLiteral("每%1").arg(unit)
                         : QStringLiteral("每%1%2").arg(interval).arg(unit);
}

QDateTime eventDateTime(const QString& text)
{
    bool ok = false;
    const qint64 ms = text.toLongLong(&ok);
    if (!ok || ms <= 0) return QDateTime();
    return fromUtcMs(ms);
}

QString eventDateDisplay(const QString& text)
{
    const QDateTime local = toAppTime(eventDateTime(text));
    return local.isValid() ? local.toString(QStringLiteral("M月d日 HH:mm")) : QString();
}

QString eventDayDisplay(const QString& text)
{
    const QDate date = QDate::fromString(text, Qt::ISODate);
    if (!date.isValid()) return QString();
    return date.toString(date.year() == toAppTime(appNow()).date().year()
        ? QStringLiteral("M月d日") : QStringLiteral("yyyy年M月d日"));
}

// 事件行时间：今天只显示时刻，其余补日期，中文可读。
QString timeLabelText(const QDateTime& occurredAt)
{
    if (!occurredAt.isValid()) return QString();
    const QDateTime local = toAppTime(occurredAt);
    const QDate today = toAppTime(appNow()).date();
    if (local.date() == today) return local.toString(QStringLiteral("HH:mm"));
    if (local.date() == today.addDays(-1))
        return QStringLiteral("昨天 ") + local.toString(QStringLiteral("HH:mm"));
    if (local.date().year() == today.year())
        return local.toString(QStringLiteral("M月d日 HH:mm"));
    return local.toString(QStringLiteral("yyyy年M月d日 HH:mm"));
}

// tags 字段是 JSON 数组快照，值是用户标签名；解析失败则整体降级，不显示原文。
QString tagsDisplay(const QString& snapshot)
{
    const QJsonDocument document = QJsonDocument::fromJson(snapshot.toUtf8());
    if (!document.isArray()) return QString();
    QStringList names;
    for (const QJsonValue& value : document.array()) {
        const QString name = value.toString().trimmed();
        if (!name.isEmpty()) names.append(name);
    }
    return names.join(QStringLiteral("、"));
}

} // namespace

// ---------------------------------------------------------------------------
// TimelineRow
// ---------------------------------------------------------------------------

ActivityTimelineWidget::TimelineRow::TimelineRow(Marker marker, const QString& summaryText,
                                                 const QString& timeText, QWidget* parent)
    : QFrame(parent), m_marker(marker)
{
    setObjectName(QStringLiteral("ActivityTimelineRow"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(TextIndent, 0, 0, RowBottomGap);
    layout->setSpacing(SummaryTimeGap);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("ActivityTimelineRowSummary"));
    // 纯文本 QLabel：所有用户内容原样展示，不存在富文本注入面。
    m_summary->setTextFormat(Qt::PlainText);
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_summary->setFont(ds::qfont(ds::FontScene::Base));            // 摘要 14px
    m_summary->setText(summaryText);
    layout->addWidget(m_summary);

    m_time = new QLabel(this);
    m_time->setObjectName(QStringLiteral("ActivityTimelineRowTime"));
    m_time->setTextFormat(Qt::PlainText);
    m_time->setFont(ds::qfont(ds::FontScene::Caption));            // 时间 12px
    m_time->setMinimumHeight(TimeLineHeight);                      // 12px / 20px 行高
    m_time->setText(timeText);
    layout->addWidget(m_time);

    applyTheme();
}

void ActivityTimelineWidget::TimelineRow::setBounds(bool first, bool terminal)
{
    if (m_first == first && m_terminal == terminal) return;
    m_first = first;
    m_terminal = terminal;
    update();
}

void ActivityTimelineWidget::TimelineRow::applyTheme()
{
    auto& theme = ThemeManager::instance();
    m_summary->setStyleSheet(QStringLiteral("QLabel{color:%1;background:transparent;}")
        .arg(theme.color(ds::ColorToken::Text).name()));
    m_time->setStyleSheet(QStringLiteral("QLabel{color:%1;background:transparent;}")
        .arg(theme.color(ds::ColorToken::TextTertiary).name()));
    update();
}

void ActivityTimelineWidget::TimelineRow::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    auto& theme = ThemeManager::instance();

    // 1px 连接线：非首行从行顶画起、非末行画到行底，穿过行间距保持连续；
    // 首行不低于圆心、末行止于圆心。
    const qreal lineX = (MarkerSize - 1) / 2.0;
    const qreal centerY = MarkerSize / 2.0;
    const qreal top = m_first ? centerY : 0.0;
    const qreal bottom = m_terminal ? centerY : height();
    if (bottom > top) {
        painter.fillRect(QRectF(lineX, top, 1.0, bottom - top),
                         theme.color(ds::ColorToken::Divider));
    }

    QColor fill;
    QString glyph;
    switch (m_marker) {
    case Marker::Create:  fill = theme.color(ds::ColorToken::Primary); glyph = QStringLiteral("ac-create"); break;
    case Marker::Edit:    fill = theme.color(ds::ColorToken::Warning); glyph = QStringLiteral("ac-edit"); break;
    case Marker::Remove:  fill = theme.color(ds::ColorToken::Danger);  glyph = QStringLiteral("ac-delete"); break;
    case Marker::Done:    fill = theme.color(ds::ColorToken::Success); glyph = QStringLiteral("selected"); break;
    case Marker::Restore: fill = theme.color(ds::ColorToken::Success); glyph = QStringLiteral("restore"); break;
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawEllipse(QRectF(0.5, 0.5, MarkerSize - 1.0, MarkerSize - 1.0));

    const QRectF glyphRect((MarkerSize - MarkerGlyphSize) / 2.0,
                           (MarkerSize - MarkerGlyphSize) / 2.0,
                           MarkerGlyphSize, MarkerGlyphSize);
    AppIcons::paint(&painter, glyph, glyphRect, theme.color(ds::ColorToken::OnPrimary));
}

// ---------------------------------------------------------------------------
// ActivityTimelineWidget
// ---------------------------------------------------------------------------

ActivityTimelineWidget::ActivityTimelineWidget(TaskStore* store, QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ActivityTimeline"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);   // 外侧横向边距由宿主表单承担
    layout->setSpacing(0);

    auto* header = new QFrame(this);
    header->setObjectName(QStringLiteral("ActivityTimelineHeader"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, HeaderTop, 0, HeaderBottom);
    headerLayout->setSpacing(4);
    m_titleLabel = new QLabel(QStringLiteral("任务动态"), header);
    m_titleLabel->setObjectName(QStringLiteral("ActivityTimelineTitle"));
    m_titleLabel->setFont(ds::qfont(ds::FontScene::Section));      // 14px / 600
    headerLayout->addWidget(m_titleLabel, 1);
    m_closeButton = new QToolButton(header);
    m_closeButton->setObjectName(QStringLiteral("ActivityTimelineClose"));
    m_closeButton->setFixedSize(CloseSize, CloseSize);
    m_closeButton->setIconSize(QSize(CloseSize - 2, CloseSize - 2));
    m_closeButton->setCursor(Qt::PointingHandCursor);
    m_closeButton->setToolTip(QStringLiteral("收起任务动态"));
    m_closeButton->setAccessibleName(m_closeButton->toolTip());
    headerLayout->addWidget(m_closeButton, 0, Qt::AlignTop);
    layout->addWidget(header);

    m_rowsContainer = new QWidget(this);
    m_rowsContainer->setObjectName(QStringLiteral("ActivityTimelineRows"));
    m_rowsLayout = new QVBoxLayout(m_rowsContainer);
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(0);   // 行距由行内底边距承担，连接线才不会断
    layout->addWidget(m_rowsContainer);

    // 组件不隐藏自己；宿主收到信号后决定隐藏。
    connect(m_closeButton, &QToolButton::clicked, this, &ActivityTimelineWidget::closeRequested);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &ActivityTimelineWidget::applyTheme);

    setStore(store);
    applyTheme();
}

void ActivityTimelineWidget::setStore(TaskStore* store)
{
    if (m_store.data() == store) return;
    if (m_store) disconnect(m_store, nullptr, this, nullptr);
    m_store = store;
    if (m_store) {
        // 只在事件属于当前实体时更新；跨实体互不串扰。
        connect(m_store, &TaskStore::eventsChanged,
                this, &ActivityTimelineWidget::onStoreEventsChanged);
    }
    refresh();
}

void ActivityTimelineWidget::setEntityId(const QString& entityId)
{
    if (entityId == m_entityId) return;   // 同 id 不重置现有行
    m_entityId = entityId;
    refresh();
}

void ActivityTimelineWidget::refresh()
{
    m_dirty = false;
    clearRows();
    if (m_store.isNull() || m_entityId.isEmpty()) return;   // 无实体即清空
    const QVector<EntityEvent> events = m_store->events(m_entityId);

    QVector<const EntityEvent*> shown;
    shown.reserve(events.size());
    for (const EntityEvent& event : events) {
        if (isVisibleActivityEvent(event)) shown.append(&event);
    }
    for (int i = shown.size() - 1; i >= 0; --i) {   // 最新在前
        const EntityEvent& event = *shown.at(i);
        Marker marker = Marker::Edit;
        const QString summary = summaryFor(event, &marker);
        auto* row = new TimelineRow(marker, summary, timeLabelText(event.occurredAt), m_rowsContainer);
        row->setBounds(i == shown.size() - 1, i == 0);
        m_rowsLayout->addWidget(row);
        m_rows.append(row);
    }
}

void ActivityTimelineWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_dirty) refresh();   // 补上隐藏期间错过的事件更新
}

void ActivityTimelineWidget::onStoreEventsChanged(const QString& entityId)
{
    if (entityId != m_entityId) return;
    if (isVisible()) refresh();
    else m_dirty = true;
}

void ActivityTimelineWidget::applyTheme()
{
    auto& theme = ThemeManager::instance();
    m_titleLabel->setStyleSheet(QStringLiteral("QLabel{color:%1;background:transparent;}")
        .arg(theme.color(ds::ColorToken::TextSecondary).name()));
    m_closeButton->setStyleSheet(QStringLiteral(
        "QToolButton{border:0;border-radius:4px;background:transparent;padding:0;}"
        "QToolButton:hover{background:%1;}")
        .arg(theme.color(ds::ColorToken::Hover).name()));
    m_closeButton->setIcon(AppIcons::icon(IconId::Close, ds::ColorToken::TextTertiary, CloseSize - 2));
    for (TimelineRow* row : m_rows) {
        if (row) row->applyTheme();
    }
}

void ActivityTimelineWidget::clearRows()
{
    qDeleteAll(m_rows);
    m_rows.clear();
}

QString ActivityTimelineWidget::summaryFor(const EntityEvent& event, Marker* marker) const
{
    const QString& type = event.type;
    const QString& field = event.field;
    const QString oldText = variantText(event.oldValue);
    const QString newText = variantText(event.newValue);
    // 存储侧新增的上下文字段：旧日志没有 context 时是空对象，自然走降级文案。
    const QJsonObject context = event.context;

    if (marker) *marker = Marker::Edit;

    if (type == QStringLiteral("created")) {
        if (marker) *marker = Marker::Create;
        const QString title = elidedPreview(event.entityTitle, PreviewMaxChars);
        return title.isEmpty() ? QStringLiteral("创建了%1").arg(kindLabel(event.entityKind))
            : QStringLiteral("创建了%1「%2」").arg(kindLabel(event.entityKind), title);
    }
    if (type == QStringLiteral("trashed")) {
        if (marker) *marker = Marker::Remove;
        return QStringLiteral("移入了回收站");
    }
    if (type == QStringLiteral("restored")) {
        if (marker) *marker = Marker::Restore;
        return QStringLiteral("从回收站恢复");
    }
    if (type == QStringLiteral("deleted")) {
        if (marker) *marker = Marker::Remove;
        return QStringLiteral("永久删除了%1").arg(kindLabel(event.entityKind));
    }
    if (type == QStringLiteral("kind_changed")) {
        if (newText == QStringLiteral("note")) return QStringLiteral("转换为了笔记");
        if (newText == QStringLiteral("task")) return QStringLiteral("转换为了任务");
        return QStringLiteral("转换了类型");
    }
    if (type == QStringLiteral("attachment_added")) {
        if (marker) *marker = Marker::Create;
        const QString name = context.value(QStringLiteral("attachmentName")).toString();
        if (!name.isEmpty())
            return QStringLiteral("添加了附件「%1」").arg(elidedPreview(name, PreviewMaxChars));
        return QStringLiteral("添加了附件");
    }
    if (type == QStringLiteral("attachment_removed")) {
        if (marker) *marker = Marker::Remove;
        const QString name = context.value(QStringLiteral("attachmentName")).toString();
        if (!name.isEmpty())
            return QStringLiteral("移除了附件「%1」").arg(elidedPreview(name, PreviewMaxChars));
        return QStringLiteral("移除了附件");
    }
    if (type == QStringLiteral("reminder_added")) {
        if (marker) *marker = Marker::Create;
        return QStringLiteral("添加了提醒");
    }
    if (type == QStringLiteral("reminder_removed")) {
        if (marker) *marker = Marker::Remove;
        return QStringLiteral("移除了提醒");
    }

    if (type != QStringLiteral("changed"))
        return QStringLiteral("更新了%1").arg(kindLabel(event.entityKind));

    if (field == QStringLiteral("title")) {
        if (newText.simplified().isEmpty()) return QStringLiteral("清空了标题");
        // 完整标题不塞进时间线：只给短预览。
        return QStringLiteral("将标题修改为「%1」").arg(elidedPreview(newText, PreviewMaxChars));
    }
    if (field == QStringLiteral("body")) {
        return newText.simplified().isEmpty() ? QStringLiteral("清空了正文")
                                              : QStringLiteral("修改了正文");
    }
    if (field == QStringLiteral("status")) {
        if (newText == QStringLiteral("completed")) {
            if (marker) *marker = Marker::Done;
            return QStringLiteral("完成了任务");
        }
        if (newText == QStringLiteral("abandoned")) return QStringLiteral("放弃了任务");
        if (oldText == QStringLiteral("completed")) return QStringLiteral("重新打开了任务");
        if (oldText == QStringLiteral("abandoned")) return QStringLiteral("恢复了任务");
        return QStringLiteral("状态：%1 → %2").arg(statusLabel(oldText), statusLabel(newText));
    }
    if (field == QStringLiteral("priority"))
        return QStringLiteral("优先级：%1 → %2").arg(priorityLabel(oldText), priorityLabel(newText));
    if (field == QStringLiteral("recurrence"))
        return QStringLiteral("重复：%1 → %2").arg(recurrenceLabel(oldText), recurrenceLabel(newText));

    if (field == QStringLiteral("due") || field == QStringLiteral("start")
        || field == QStringLiteral("allDayDate") || field == QStringLiteral("allDayEndDate")) {
        const QString label = field == QStringLiteral("due") ? QStringLiteral("截止时间")
            : field == QStringLiteral("start") ? QStringLiteral("开始时间")
            : field == QStringLiteral("allDayEndDate") ? QStringLiteral("结束日期") : QStringLiteral("日期");
        QString from;
        QString to;
        if (field == QStringLiteral("allDayDate") || field == QStringLiteral("allDayEndDate")) {
            from = eventDayDisplay(oldText);
            to = eventDayDisplay(newText);
        } else {
            from = eventDateDisplay(oldText);
            to = eventDateDisplay(newText);
        }
        if (to.isEmpty()) return QStringLiteral("清除了%1").arg(label);
        if (from.isEmpty()) return QStringLiteral("设置了%1 %2").arg(label, to);
        return QStringLiteral("%1：%2 → %3").arg(label, from, to);
    }

    if (field == QStringLiteral("parent")) {
        const QString newParent = context.value(QStringLiteral("newParentTitle")).toString();
        const QString oldParent = context.value(QStringLiteral("oldParentTitle")).toString();
        if (!newParent.isEmpty()) {
            return oldParent.isEmpty()
                ? QStringLiteral("添加了主任务「%1」").arg(elidedPreview(newParent, PreviewMaxChars))
                : QStringLiteral("把主任务改为「%1」").arg(elidedPreview(newParent, PreviewMaxChars));
        }
        if (!oldParent.isEmpty())
            return QStringLiteral("移除了主任务「%1」").arg(elidedPreview(oldParent, PreviewMaxChars));
        return newText.isEmpty() ? QStringLiteral("移除了主任务")
                                  : QStringLiteral("添加了主任务");
    }
    if (field == QStringLiteral("list")) {
        const QString newList = context.value(QStringLiteral("newListName")).toString();
        const QString oldList = context.value(QStringLiteral("oldListName")).toString();
        if (!newList.isEmpty())
            return QStringLiteral("移动到了清单「%1」").arg(elidedPreview(newList, PreviewMaxChars));
        if (!oldList.isEmpty())
            return QStringLiteral("移出了清单「%1」").arg(elidedPreview(oldList, PreviewMaxChars));
        return QStringLiteral("移动了清单");
    }
    if (field == QStringLiteral("listArchived")) {
        const bool archived = !newText.isEmpty() && newText != QLatin1String("0");
        const QString name = elidedPreview(context.value(QStringLiteral("listName")).toString(), PreviewMaxChars);
        if (name.isEmpty()) return archived ? QStringLiteral("归档了清单") : QStringLiteral("取消了清单归档");
        return archived ? QStringLiteral("归档了清单「%1」").arg(name)
                        : QStringLiteral("取消了清单「%1」的归档").arg(name);
    }
    if (field == QStringLiteral("attachment") || field.startsWith(QStringLiteral("attachment."))) {
        const QString name = context.value(QStringLiteral("attachmentName")).toString();
        if (!name.isEmpty())
            return QStringLiteral("更新了附件「%1」").arg(elidedPreview(name, PreviewMaxChars));
        return QStringLiteral("更新了附件");
    }
    if (field == QStringLiteral("tags")) {
        const QString from = tagsDisplay(oldText);
        const QString to = tagsDisplay(newText);
        if (from.isEmpty() && to.isEmpty()) return QStringLiteral("修改了标签");
        if (from.isEmpty()) return QStringLiteral("添加了标签「%1」").arg(elidedPreview(to, PreviewMaxChars));
        if (to.isEmpty()) return QStringLiteral("移除了标签「%1」").arg(elidedPreview(from, PreviewMaxChars));
        return QStringLiteral("标签：%1 → %2").arg(elidedPreview(from, PreviewMaxChars),
                                                  elidedPreview(to, PreviewMaxChars));
    }
    if (field == QStringLiteral("reminder")) return QStringLiteral("更新了提醒");
    if (field.startsWith(QStringLiteral("reminder."))) return QStringLiteral("修改了提醒时间");
    if (field == QStringLiteral("timeZone")) return QStringLiteral("修改了时区");

    // 未知字段一律通用文案：绝不显示 UUID、NULL 或批次号。
    return QStringLiteral("更新了%1").arg(kindLabel(event.entityKind));
}

} // namespace kl
