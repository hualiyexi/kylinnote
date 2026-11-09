#pragma once
// 悬浮格式面板（"卡片岛"）与选中文字气泡工具条。
//
// 两者只负责 UI 呈现与定位：具体的格式逻辑由 MarkdownEditor::applyFormat() 提供，
// 图标取自 AppIcons，语义色全部经 ThemeManager 在运行时取用（不硬编码颜色），
// 主题切换时重新取色并重绘图标 / 重刷样式表。
#include <QFrame>
#include <QPointer>
#include <QVector>

class QEvent;
class QFocusEvent;
class QToolButton;
class QWidget;

namespace kl {

class MarkdownEditor;

// 底栏「A」按钮上方弹出的圆角卡片，列出 MarkdownEditor::allFormats() 的全部动作：
// 高频动作一行，分隔线以下按 2 列网格排布其余动作。
class FormatPanel : public QFrame {
    Q_OBJECT
public:
    explicit FormatPanel(MarkdownEditor* editor, QWidget* parent = nullptr);

    // 在 anchor 上方弹出（带轻阴影、圆角 12px），并夹到当前屏幕可见区域内。
    void popupAbove(QWidget* anchor);

protected:
    void focusOutEvent(QFocusEvent* event) override;

private:
    void buildUi();
    void applyTheme();

    MarkdownEditor* m_editor = nullptr;
    QWidget* m_card = nullptr;
    QVector<QToolButton*> m_buttons;
};

// 选中正文文字时浮出的高频格式气泡条，位置跟随选区。
// 独立 Qt::Tool 窗口，不抢焦点，以免清掉 QTextEdit 的选区。
class SelectionBubble : public QFrame {
    Q_OBJECT
public:
    explicit SelectionBubble(MarkdownEditor* editor, QWidget* parent = nullptr);

private slots:
    void onSelectionChanged(bool hasSelection);
    // 依据当前选区重新定位；选区失效或滚出视口时隐藏气泡。
    void reposition();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildUi();
    void applyTheme();
    void openMorePanel();
    // 计算并 move 到选区上方；返回 false 表示选区失效或滚出视口。
    bool updatePosition();

    MarkdownEditor* m_editor = nullptr;
    QWidget* m_card = nullptr;
    QToolButton* m_moreButton = nullptr;
    QVector<QToolButton*> m_buttons;
    QPointer<FormatPanel> m_panel;
    bool m_suppressSelection = false;
};

}  // namespace kl
