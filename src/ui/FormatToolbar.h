#pragma once
// 单行悬浮格式岛与选中文字气泡。格式操作统一交给 MarkdownEditor，
// 图标、色板和阴影使用设计系统的语义色；弹窗始终受所属详情面板约束。
#include <QFrame>
#include <QPointer>
#include <QTextCursor>
#include <QVector>

class QAction;
class QEvent;
class QHideEvent;
class QKeyEvent;
class QMenu;
class QMouseEvent;
class QToolButton;
class QWidget;

namespace kl {

class MarkdownEditor;

class FormatPanel : public QFrame {
    Q_OBJECT
public:
    explicit FormatPanel(MarkdownEditor* editor, QWidget* parent = nullptr);
    void popupAbove(QWidget* anchor);

signals:
    void formatApplied();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    struct OverflowItem {
        QWidget* widget;
        QAction* action;
    };

    void buildUi();
    void applyTheme();
    void updateHighlight();
    void adaptToWidth(int width);
    bool updatePosition();
    void scheduleReposition();
    bool beginAction(bool editing = true);
    void finishAction(bool closePanel = false);

    QPointer<MarkdownEditor> m_editor;
    QPointer<QWidget> m_anchor;
    QRect m_anchorGeometry;
    QWidget* m_card = nullptr;
    QWidget* m_highlightGroup = nullptr;
    QToolButton* m_highlight = nullptr;
    QToolButton* m_paletteButton = nullptr;
    QMenu* m_palette = nullptr;
    QMenu* m_overflow = nullptr;
    QAction* m_overflowSeparator = nullptr;
    QVector<QToolButton*> m_buttons;
    QVector<QAction*> m_menuActions;
    QVector<QAction*> m_swatches;
    QVector<QWidget*> m_dividers;
    QVector<OverflowItem> m_overflowItems;
    QTextCursor m_selection;
    bool m_applying = false;
    bool m_repositionPending = false;
};

// 不抢正文焦点；选区滚出所有祖先的裁剪区域或所属窗口隐藏时立即收起。
class SelectionBubble : public QFrame {
    Q_OBJECT
public:
    explicit SelectionBubble(MarkdownEditor* editor, QWidget* parent = nullptr);

private slots:
    void onSelectionChanged(bool hasSelection);
    void reposition();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildUi();
    void applyTheme();
    void openMorePanel();
    bool updatePosition();
    void dismiss();

    QPointer<MarkdownEditor> m_editor;
    QWidget* m_card = nullptr;
    QToolButton* m_moreButton = nullptr;
    QVector<QToolButton*> m_buttons;
    QPointer<FormatPanel> m_panel;
    QTextCursor m_selection;
    bool m_suppressSelection = false;
};

}  // namespace kl
