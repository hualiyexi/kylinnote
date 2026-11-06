#include "TaskPanel.h"
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCursor>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include "../app/TaskStore.h"
#include "../platform/Autostart.h"
#include "AppStyle.h"
#include "TaskDelegate.h"
#include "TaskDetailWidget.h"
#include "TaskListModel.h"
namespace kn {
namespace {
// 侧栏顶部的小应用标记：蓝色圆角方块 + 白色对勾，本地矢量绘制。
QPixmap brandMark(int size) {
    const qreal dpr = 2.0;
    QPixmap pixmap(qRound(size * dpr), qRound(size * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.scale(dpr, dpr);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(AppStyle::color(AppStyle::Color::Accent));
    p.drawRoundedRect(QRectF(0.5, 0.5, size - 1.0, size - 1.0), size * 0.28, size * 0.28);
    p.setPen(QPen(AppStyle::color(AppStyle::Color::OnAccent),
                  size * 0.10,
                  Qt::SolidLine,
                  Qt::RoundCap,
                  Qt::RoundJoin));
    p.drawPolyline(QPolygonF() << QPointF(size * 0.26, size * 0.52) << QPointF(size * 0.44, size * 0.69)
                               << QPointF(size * 0.75, size * 0.32));
    return pixmap;
}
}  // namespace
TaskPanel::TaskPanel(TaskStore* store, QWidget* parent) : QMainWindow(parent), m_store(store) {
    setObjectName(QStringLiteral("TaskPanel"));
    setWindowTitle(QStringLiteral("KylinNote · 我的待办"));
    setMinimumSize(960, 620);
    resize(1240, 820);
    buildUi();
    rebuildNavigation();
    QSettings settings(
        QSettings::IniFormat, QSettings::UserScope, QStringLiteral("kylinnote"), QStringLiteral("panel"));
    restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());
    m_splitter->restoreState(settings.value(QStringLiteral("splitter")).toByteArray());
    if (QGuiApplication::primaryScreen()) {
        const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
        resize(size().boundedTo(available.size()));
        if (!available.intersects(frameGeometry()))
            move(available.topLeft() + QPoint(20, 20));
    }
    connect(store, &TaskStore::taskChanged, this, [this]() {
        m_error->hide();
        if (QWidget* host = m_error->parentWidget())
            host->hide();
        rebuildNavigation();
    });
    connect(store, &TaskStore::groupsChanged, this, &TaskPanel::rebuildNavigation);
    connect(store, &TaskStore::tasksReset, this, &TaskPanel::rebuildNavigation);
    connect(store, &TaskStore::errorOccurred, this, &TaskPanel::showError);
    auto* clock = new QTimer(this);
    clock->setInterval(60000);
    connect(clock, &QTimer::timeout, m_model, &TaskListModel::refresh);
    clock->start();
}
void TaskPanel::buildUi() {
    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto* errorHost = new QWidget(this);
    auto* errorLayout = new QHBoxLayout(errorHost);
    errorLayout->setContentsMargins(24, 12, 24, 0);
    m_error = new QLabel(errorHost);
    m_error->setTextFormat(Qt::PlainText);
    m_error->setObjectName(QStringLiteral("ErrorBanner"));
    m_error->setWordWrap(true);
    m_error->setTextInteractionFlags(Qt::TextSelectableByMouse);
    errorLayout->addWidget(m_error);
    errorHost->hide();
    root->addWidget(errorHost);
    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    root->addLayout(body, 1);
    auto* sidebar = new QFrame(this);
    sidebar->setObjectName(QStringLiteral("Sidebar"));
    sidebar->setFixedWidth(AppStyle::metric(AppStyle::Metric::SidebarWidth));
    auto* side = new QVBoxLayout(sidebar);
    side->setContentsMargins(8, 14, 8, 12);
    side->setSpacing(2);
    auto* brandRow = new QHBoxLayout;
    brandRow->setContentsMargins(8, 2, 4, 14);
    brandRow->setSpacing(8);
    auto* mark = new QLabel(sidebar);
    mark->setFixedSize(22, 22);
    mark->setPixmap(brandMark(22));
    brandRow->addWidget(mark);
    auto* brand = new QLabel(QStringLiteral("KylinNote"), sidebar);
    brand->setFont(AppStyle::font(AppStyle::Font::Brand));
    brandRow->addWidget(brand);
    brandRow->addStretch(1);
    side->addLayout(brandRow);
    m_navigation = new QListWidget(sidebar);
    m_navigation->setObjectName(QStringLiteral("Navigation"));
    m_navigation->setItemDelegate(new NavigationDelegate(m_navigation));
    m_navigation->setMouseTracking(true);
    m_navigation->viewport()->setAttribute(Qt::WA_Hover, true);
    m_navigation->setAccessibleName(QStringLiteral("任务导航"));
    side->addWidget(m_navigation, 1);
    auto* groups = new QPushButton(QStringLiteral("管理分组"), sidebar);
    groups->setProperty("quiet", true);
    groups->setCursor(Qt::PointingHandCursor);
    side->addWidget(groups);
    connect(groups, &QPushButton::clicked, this, &TaskPanel::groupMenu);
    auto* backup = new QPushButton(QStringLiteral("备份与设置"), sidebar);
    backup->setProperty("quiet", true);
    backup->setCursor(Qt::PointingHandCursor);
    side->addWidget(backup);
    connect(backup, &QPushButton::clicked, this, &TaskPanel::backupMenu);
    body->addWidget(sidebar);
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setHandleWidth(4);
    body->addWidget(m_splitter, 1);
    auto* middle = new QFrame(this);
    middle->setObjectName(QStringLiteral("ListPane"));
    middle->setMinimumWidth(300);
    auto* center = new QVBoxLayout(middle);
    const int margin = AppStyle::metric(AppStyle::Metric::ContentMargin);
    center->setContentsMargins(margin, 18, margin, 10);
    center->setSpacing(12);
    // 头部整合：标题 + 计数 + 排序，不再单独显示大号日期。
    auto* header = new QHBoxLayout;
    header->setSpacing(10);
    m_heading = new QLabel(QStringLiteral("全部待办"), middle);
    m_heading->setFont(AppStyle::font(AppStyle::Font::Heading));
    header->addWidget(m_heading, 0, Qt::AlignBottom);
    m_count = new QLabel(middle);
    m_count->setProperty("muted", true);
    m_count->setFont(AppStyle::font(AppStyle::Font::Small));
    m_count->setContentsMargins(0, 0, 0, 5);
    header->addWidget(m_count, 0, Qt::AlignBottom);
    header->addStretch(1);
    m_sort = new QComboBox(middle);
    m_sort->setObjectName(QStringLiteral("TaskSort"));
    m_sort->setAccessibleName(QStringLiteral("任务排序方式"));
    m_sort->addItems({QStringLiteral("按安排时间"),
                      QStringLiteral("按优先级"),
                      QStringLiteral("按创建时间"),
                      QStringLiteral("按标题")});
    m_sort->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_sort->setMinimumWidth(120);
    m_sort->setFixedHeight(32);
    header->addWidget(m_sort, 0, Qt::AlignVCenter);
    center->addLayout(header);
    m_search = new QLineEdit(middle);
    m_search->setObjectName(QStringLiteral("TaskSearch"));
    m_search->setPlaceholderText(QStringLiteral("搜索标题、说明、标签和子任务  Ctrl+F"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedHeight(AppStyle::metric(AppStyle::Metric::ControlHeight));
    m_search->setAccessibleName(QStringLiteral("搜索任务"));
    center->addWidget(m_search);
    auto* quick = new QHBoxLayout;
    quick->setSpacing(8);
    m_quick = new QLineEdit(middle);
    m_quick->setObjectName(QStringLiteral("QuickAdd"));
    m_quick->setPlaceholderText(QStringLiteral("添加一项待办，按回车确认"));
    m_quick->setMaxLength(500);
    m_quick->setFixedHeight(AppStyle::metric(AppStyle::Metric::ControlHeight));
    m_quick->setAccessibleName(QStringLiteral("快速添加任务"));
    auto* add = new QPushButton(QStringLiteral("添加"), middle);
    add->setProperty("primary", true);
    add->setObjectName(QStringLiteral("QuickAddButton"));
    add->setFixedHeight(32);
    add->setMinimumWidth(68);
    add->setCursor(Qt::PointingHandCursor);
    quick->addWidget(m_quick, 1);
    quick->addWidget(add);
    center->addLayout(quick);
    connect(add, &QPushButton::clicked, this, &TaskPanel::createTask);
    connect(m_quick, &QLineEdit::returnPressed, this, &TaskPanel::createTask);
    m_model = new TaskListModel(m_store, this);
    m_list = new QListView(middle);
    m_list->setObjectName(QStringLiteral("TaskList"));
    m_list->setModel(m_model);
    m_list->setItemDelegate(new TaskDelegate(m_list));
    m_list->setMouseTracking(true);
    m_list->viewport()->setAttribute(Qt::WA_Hover, true);
    m_list->setUniformItemSizes(true);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setAccessibleName(QStringLiteral("任务列表"));
    m_empty = new QLabel(QStringLiteral("这里还没有任务\n在上方输入一件要做的事，开始记录。"), middle);
    m_empty->setProperty("muted", true);
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignCenter);
    // 空态与列表同区域切换，空列表时提示居中而不是沉底。
    m_listStack = new QStackedWidget(middle);
    m_listStack->addWidget(m_list);
    m_listStack->addWidget(m_empty);
    center->addWidget(m_listStack, 1);
    m_splitter->addWidget(middle);
    auto* detailFrame = new QFrame(this);
    detailFrame->setObjectName(QStringLiteral("DetailFrame"));
    detailFrame->setMinimumWidth(370);
    auto* detailLayout = new QVBoxLayout(detailFrame);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    m_detail = new TaskDetailWidget(m_store, detailFrame);
    detailLayout->addWidget(m_detail);
    m_splitter->addWidget(detailFrame);
    m_splitter->setSizes({560, 440});
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setChildrenCollapsible(false);
    connect(m_model, &QAbstractItemModel::modelAboutToBeReset, this, [this]() { m_resetting = true; });
    connect(m_model, &QAbstractItemModel::modelReset, this, [this]() {
        const QModelIndex index = m_model->indexForTask(m_selectedId);
        if (index.isValid())
            m_list->setCurrentIndex(index);
        m_count->setText(QStringLiteral("%1 项任务").arg(m_model->rowCount()));
        m_listStack->setCurrentWidget(m_model->rowCount() == 0 ? static_cast<QWidget*>(m_empty)
                                                               : static_cast<QWidget*>(m_list));
        m_resetting = false;
    });
    connect(m_list->selectionModel(),
            &QItemSelectionModel::currentChanged,
            this,
            [this](const QModelIndex& current) {
                if (m_resetting)
                    return;
                const qint64 id = m_model->taskId(current);
                if (m_detail->setTask(id))
                    m_selectedId = id;
                else {
                    const QSignalBlocker blocker(m_list->selectionModel());
                    m_list->setCurrentIndex(m_model->indexForTask(m_selectedId));
                }
            });
    connect(m_list, &QListView::customContextMenuRequested, this, &TaskPanel::showTaskMenu);
    connect(m_search, &QLineEdit::textChanged, this, [this]() { applyFilter(); });
    connect(m_sort, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { applyFilter(); });
    connect(m_navigation, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_rebuilding || row < 0)
            return;
        if (!m_detail->flush()) {
            rebuildNavigation();
            return;
        }
        auto* item = m_navigation->item(row);
        if (!item)
            return;
        m_filter.view = static_cast<TaskView>(item->data(Qt::UserRole).toInt());
        m_filter.groupId = item->data(Qt::UserRole + 1).toLongLong();
        m_heading->setText(item->data(Qt::UserRole + 2).toString());
        applyFilter();
    });
    auto* newShortcut = new QShortcut(QKeySequence::New, this);
    connect(newShortcut, &QShortcut::activated, this, &TaskPanel::focusQuickAdd);
    auto* findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated, m_search, QOverload<>::of(&QWidget::setFocus));
    auto* saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, m_detail, &TaskDetailWidget::flush);
    statusBar()->setSizeGripEnabled(false);
    statusBar()->setFont(AppStyle::font(AppStyle::Font::Small));
    statusBar()->showMessage(QStringLiteral("本机保存 · 无需联网"));
}
void TaskPanel::rebuildNavigation() {
    m_rebuilding = true;
    const QSignalBlocker blocker(m_navigation);
    m_navigation->clear();
    int selected = 0;
    auto section = [this](const QString& title) {
        auto* item = new QListWidgetItem(title, m_navigation);
        item->setFlags(Qt::NoItemFlags);  // 分区标题不可选中，键盘上下键自动跳过
        item->setData(NavigationDelegate::SectionRole, true);
    };
    auto add = [this, &selected](const QString& label, TaskView view, NavIcon icon, qint64 group = 0) {
        TaskFilter filter;
        filter.view = view;
        filter.groupId = group;
        int count = 0;
        for (const auto& task : m_store->tasks())
            if (filter.matches(task, QDateTime::currentDateTime()))
                ++count;
        auto* item = new QListWidgetItem(label, m_navigation);
        item->setData(Qt::UserRole, static_cast<int>(view));
        item->setData(Qt::UserRole + 1, group);
        item->setData(Qt::UserRole + 2, label);
        item->setData(NavigationDelegate::CountRole, count);  // 数量独立角色，右对齐绘制
        item->setData(NavigationDelegate::IconRole, static_cast<int>(icon));
        item->setToolTip(label);
        if (view == m_filter.view && (view != TaskView::Group || group == m_filter.groupId))
            selected = m_navigation->count() - 1;
    };
    section(QStringLiteral("智能列表"));
    add(QStringLiteral("全部待办"), TaskView::All, NavIcon::Inbox);
    add(QStringLiteral("今天"), TaskView::Today, NavIcon::Today);
    add(QStringLiteral("七天内"), TaskView::Upcoming, NavIcon::Upcoming);
    add(QStringLiteral("已逾期"), TaskView::Overdue, NavIcon::Overdue);
    add(QStringLiteral("重要与星标"), TaskView::Important, NavIcon::Star);
    add(QStringLiteral("已贴桌面"), TaskView::Pinned, NavIcon::Pin);
    add(QStringLiteral("已完成"), TaskView::Completed, NavIcon::Done);
    add(QStringLiteral("回收站"), TaskView::Trash, NavIcon::Trash);
    section(QStringLiteral("分组"));
    add(QStringLiteral("未分组"), TaskView::Group, NavIcon::Folder);
    for (const auto& group : m_store->groups())
        add(group.name, TaskView::Group, NavIcon::Folder, group.id);
    m_navigation->setCurrentRow(selected);
    m_rebuilding = false;
    m_count->setText(QStringLiteral("%1 项任务").arg(m_model->rowCount()));
    m_listStack->setCurrentWidget(m_model->rowCount() == 0 ? static_cast<QWidget*>(m_empty)
                                                           : static_cast<QWidget*>(m_list));
}
void TaskPanel::applyFilter() {
    m_filter.search = m_search->text();
    m_filter.sort = static_cast<TaskSort>(m_sort->currentIndex());
    m_model->setFilter(m_filter);
}
void TaskPanel::createTask() {
    const QString title = m_quick->text().trimmed();
    if (title.isEmpty()) {
        m_quick->setFocus();
        return;
    }
    if (!m_detail->flush())
        return;
    const qint64 id = m_store->createTask(title, m_filter.view == TaskView::Group ? m_filter.groupId : 0);
    if (!id)
        return;
    m_quick->clear();
    showTask(id);
    m_detail->focusTitle();
}
void TaskPanel::showTask(qint64 id) {
    if (!m_detail->flush()) {
        showNormal();
        raise();
        activateWindow();
        return;
    }
    if (id > 0) {
        const Task task = m_store->task(id);
        if (!task.id)
            return;
        if (!m_filter.matches(task, QDateTime::currentDateTime())) {
            m_filter.view =
                task.deleted ? TaskView::Trash : (task.completed ? TaskView::Completed : TaskView::All);
            m_search->clear();
            m_filter.search.clear();
            m_heading->setText(
                task.deleted ? QStringLiteral("回收站")
                             : (task.completed ? QStringLiteral("已完成") : QStringLiteral("全部待办")));
            applyFilter();
            rebuildNavigation();
        }
        m_selectedId = id;
        const auto index = m_model->indexForTask(id);
        m_list->setCurrentIndex(index);
        m_list->scrollTo(index);
        m_detail->setTask(id);
    }
    showNormal();
    raise();
    activateWindow();
}
void TaskPanel::focusQuickAdd() {
    showTask();
    m_quick->setFocus();
}
bool TaskPanel::flush() {
    return m_detail->flush();
}
void TaskPanel::showError(const QString& message) {
    m_error->setText(message);
    m_error->show();
    if (QWidget* host = m_error->parentWidget())
        host->show();
    statusBar()->showMessage(message);
}
void TaskPanel::showTaskMenu(const QPoint& position) {
    const qint64 id = m_model->taskId(m_list->indexAt(position));
    if (!id)
        return;
    const Task task = m_store->task(id);
    QMenu menu(this);
    if (task.deleted) {
        connect(menu.addAction(QStringLiteral("恢复任务")), &QAction::triggered, this, [this, id]() {
            m_store->restoreTask(id);
        });
    } else {
        connect(menu.addAction(task.completed ? QStringLiteral("设为未完成") : QStringLiteral("完成任务")),
                &QAction::triggered,
                this,
                [this, id, task]() {
                    if (flush())
                        m_store->setCompleted(id, !task.completed);
                });
        connect(menu.addAction(task.pinned ? QStringLiteral("取消贴到桌面") : QStringLiteral("贴到桌面")),
                &QAction::triggered,
                this,
                [this, id, task]() {
                    if (flush())
                        m_store->setPinned(id, !task.pinned);
                });
        connect(menu.addAction(QStringLiteral("复制为新任务")), &QAction::triggered, this, [this, id]() {
            if (!flush())
                return;
            Task copy = m_store->task(id);
            copy.id = 0;
            copy.title += QStringLiteral("（副本）");
            copy.completed = false;
            copy.deleted = false;
            copy.pinned = false;
            copy.completedAt = QDateTime();
            copy.notifiedAt = QDateTime();
            copy.reminderAt = QDateTime();
            copy.createdAt = QDateTime();
            copy.updatedAt = QDateTime();
            // 通过 createTask 获得独立任务身份，防止携带重复任务的来源链接。
            const qint64 newId = m_store->createTask(copy.title, copy.groupId);
            if (!newId)
                return;
            const Task created = m_store->task(newId);
            copy.id = newId;
            copy.createdAt = created.createdAt;
            if (m_store->saveTask(copy))
                showTask(newId);
        });
        menu.addSeparator();
        connect(menu.addAction(QStringLiteral("移到回收站")), &QAction::triggered, this, [this, id]() {
            if (flush())
                m_store->trashTask(id);
        });
    }
    menu.exec(m_list->viewport()->mapToGlobal(position));
}
void TaskPanel::groupMenu() {
    QMenu menu(this);
    const qint64 groupId = m_filter.view == TaskView::Group ? m_filter.groupId : 0;
    connect(menu.addAction(QStringLiteral("新建分组")), &QAction::triggered, this, [this]() {
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, QStringLiteral("新建分组"), QStringLiteral("名称："), QLineEdit::Normal, QString(), &ok);
        if (ok && !name.trimmed().isEmpty())
            m_store->createGroup(name.trimmed());
    });
    auto* rename = menu.addAction(QStringLiteral("重命名当前分组"));
    rename->setEnabled(groupId > 0);
    connect(rename, &QAction::triggered, this, [this, groupId]() {
        bool ok = false;
        const QString name = QInputDialog::getText(this,
                                                   QStringLiteral("重命名分组"),
                                                   QStringLiteral("新名称："),
                                                   QLineEdit::Normal,
                                                   QString(),
                                                   &ok);
        if (ok && !name.trimmed().isEmpty())
            m_store->renameGroup(groupId, name.trimmed());
    });
    auto* remove = menu.addAction(QStringLiteral("删除当前分组"));
    remove->setEnabled(groupId > 0);
    connect(remove, &QAction::triggered, this, [this, groupId]() {
        if (!flush())
            return;
        if (QMessageBox::question(this,
                                  QStringLiteral("删除分组"),
                                  QStringLiteral("删除分组后，其中的任务会移到“未分组”，任务内容会保留。")) ==
            QMessageBox::Yes) {
            if (m_store->deleteGroup(groupId)) {
                m_filter.view = TaskView::All;
                m_heading->setText(QStringLiteral("全部待办"));
                applyFilter();
                rebuildNavigation();
            }
        }
    });
    menu.exec(QCursor::pos());
}
void TaskPanel::backupMenu() {
    QMenu menu(this);
    connect(menu.addAction(QStringLiteral("导出备份…")), &QAction::triggered, this, [this]() {
        if (!flush())
            return;
        const QString path = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("导出备份"),
            QStringLiteral("KylinNote-%1.knb").arg(QDate::currentDate().toString(Qt::ISODate)),
            QStringLiteral("KylinNote 备份（含附件） (*.knb)"));
        if (!path.isEmpty() && m_store->exportBackup(path))
            statusBar()->showMessage(QStringLiteral("备份已保存：") + path, 10000);
    });
    connect(menu.addAction(QStringLiteral("导入备份…")), &QAction::triggered, this, [this]() {
        if (!flush())
            return;
        const QString path = QFileDialog::getOpenFileName(this,
                                                          QStringLiteral("导入备份（作为新任务合并）"),
                                                          QString(),
                                                          QStringLiteral("KylinNote 备份 (*.knb);;旧版 JSON 备份 (*.json)"));
        if (path.isEmpty())
            return;
        if (QMessageBox::question(
                this,
                QStringLiteral("导入备份"),
                QStringLiteral(
                    "备份中的任务将作为新任务加入，现有任务会保留。重复导入会产生副本，是否继续？")) ==
            QMessageBox::Yes)
            if (m_store->importBackup(path))
                statusBar()->showMessage(QStringLiteral("备份导入完成。"), 8000);
    });
    menu.addSeparator();
    auto* startup = menu.addAction(QStringLiteral("登录时自动启动"));
    startup->setCheckable(true);
    startup->setChecked(Autostart::isInstalled());
    connect(startup, &QAction::triggered, this, [this](bool enabled) {
        QString error;
        if (!Autostart::setEnabled(enabled, &error))
            showError(error);
    });
    connect(menu.addAction(QStringLiteral("打开数据位置")), &QAction::triggered, this, []() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(dataDirPath()));
    });
    menu.addSeparator();
    connect(menu.addAction(QStringLiteral("退出 KylinNote")), &QAction::triggered, this, [this]() {
        if (flush())
            emit quitRequested();
    });
    menu.exec(QCursor::pos());
}
void TaskPanel::closeEvent(QCloseEvent* event) {
    if (!flush()) {
        event->ignore();
        return;
    }
    QSettings settings(
        QSettings::IniFormat, QSettings::UserScope, QStringLiteral("kylinnote"), QStringLiteral("panel"));
    settings.setValue(QStringLiteral("geometry"), saveGeometry());
    settings.setValue(QStringLiteral("splitter"), m_splitter->saveState());
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        hide();
        event->ignore();
    } else {
        event->accept();
        emit quitRequested();
    }
}
}  // namespace kn
