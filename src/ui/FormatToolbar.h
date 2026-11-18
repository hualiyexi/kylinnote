#pragma once
// 单行格式岛与选中文字气泡复用编辑器动作和列表菜单；选区保存为源码偏移，
// 不持有会在 FocusOut 呈现转换后失效的 QTextCursor。
#include <QColor>
#include <QFrame>
#include <QImage>
#include <QPair>
#include <QPointer>
#include <QVector>

class QAction;
class QEvent;
class QHideEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QToolButton;
class QWidget;

namespace kl {

class MarkdownEditor;
class PopupMenu;

struct FormatSelection {
    QPair<int, int> offsets;
    quint64 generation = 0;
    bool valid = false;

    void capture(MarkdownEditor* editor);
    bool current(MarkdownEditor* editor) const;
    bool restore(MarkdownEditor* editor) const;
    void clear() { valid = false; }
};

class FormatPanel : public QFrame {
    Q_OBJECT
public:
    explicit FormatPanel(MarkdownEditor* editor, QWidget* parent = nullptr);
    void popupAbove(QWidget* anchor);

signals:
    void formatApplied();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
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
    void wireMenu(PopupMenu* menu);
    void openMenu(PopupMenu* menu, QWidget* anchor);
    bool menusActive() const;
    bool beginAction(bool editing = true);
    void finishAction(bool closePanel = false);

    QPointer<MarkdownEditor> m_editor;
    QPointer<QWidget> m_anchor;
    QRect m_anchorGeometry;
    QWidget* m_card = nullptr;
    QWidget* m_highlightGroup = nullptr;
    QToolButton* m_highlight = nullptr;
    QToolButton* m_paletteButton = nullptr;
    PopupMenu* m_palette = nullptr;
    PopupMenu* m_overflow = nullptr;
    PopupMenu* m_blockStyles = nullptr;
    QAction* m_overflowSeparator = nullptr;
    QVector<QToolButton*> m_buttons;
    QVector<QAction*> m_menuActions;
    QVector<QAction*> m_swatches;
    QVector<QWidget*> m_dividers;
    QVector<OverflowItem> m_overflowItems;
    FormatSelection m_selection;
    QImage m_shadow;
    QColor m_shadowColor;
    bool m_applying = false;
    bool m_menuActive = false;
    bool m_repositionPending = false;
};

// 不抢正文焦点；窄边界把动作移入同一个纵向菜单，选区滚出祖先裁剪区域时收起。
class SelectionBubble : public QFrame {
    Q_OBJECT
public:
    explicit SelectionBubble(MarkdownEditor* editor, QWidget* parent = nullptr);

private slots:
    void onSelectionChanged(bool hasSelection);
    void reposition();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    struct OverflowItem {
        QWidget* widget;
        QAction* action;
    };

    void buildUi();
    void applyTheme();
    void adaptToWidth(int width);
    void openMoreMenu();
    void wireMenu(PopupMenu* menu);
    void openMenu(PopupMenu* menu, QWidget* anchor);
    bool menusActive() const;
    bool beginAction(bool editing = true);
    void finishAction();
    void scheduleReposition();
    bool updatePosition();
    void dismiss();

    QPointer<MarkdownEditor> m_editor;
    QWidget* m_card = nullptr;
    QWidget* m_divider = nullptr;
    QToolButton* m_textType = nullptr;
    QToolButton* m_moreButton = nullptr;
    PopupMenu* m_overflow = nullptr;
    PopupMenu* m_blockStyles = nullptr;
    QAction* m_overflowSeparator = nullptr;
    QAction* m_typeOverflow = nullptr;
    QVector<QToolButton*> m_buttons;
    QVector<QAction*> m_menuActions;
    QVector<OverflowItem> m_overflowItems;
    FormatSelection m_selection;
    QImage m_shadow;
    QColor m_shadowColor;
    bool m_applying = false;
    bool m_menuActive = false;
    bool m_suppressSelection = false;
    bool m_repositionPending = false;
};

}  // namespace kl
