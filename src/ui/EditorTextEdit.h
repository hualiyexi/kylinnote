#pragma once

#include <QElapsedTimer>
#include <QTextEdit>
#include <functional>
#include "design/DesignTokens.h"

class QToolButton;

namespace kl {

constexpr int EditorHorizontalRuleProperty = QTextFormat::UserProperty + 45;

// A real QTextEdit gutter. The indicator never overlays editable text or forwards
// background clicks, and source-aware MIME data keeps links intact on local paste.
class EditorTextEdit : public QTextEdit {
    Q_OBJECT
public:
    explicit EditorTextEdit(QWidget* parent = nullptr);
    ~EditorTextEdit() override;
    void setBlockGutterEnabled(bool enabled);
    void setThemeOverride(ds::Theme theme);
    void refreshBlockIndicator();
    QRect checkBoxRect(const QTextBlock& block) const;
    void breakTyping() { m_typingEnd = -1; m_typingTimer.invalidate(); }
    std::function<QString()> selectedSource;
    std::function<void(const QString&)> insertMarkdown;
    std::function<int()> sourceCursorPosition;
    std::function<void()> finishEdit;
    std::function<void(bool)> editCompleted;
    std::function<void(bool)> historyRequested;
    std::function<bool(bool)> historyAvailable;
    std::function<void(const QTextBlockFormat&)> paragraphInserted;
    std::function<bool()> presentationChanging;
    std::function<bool(int)> blockInsertionAllowed;

signals:
    void blockMenuRequested(int block, QRect globalAnchor);
    void compositionChanged(bool active);

protected:
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    QMimeData* createMimeDataFromSelection() const override;
    void insertFromMimeData(const QMimeData* source) override;

private:
    QRectF blockFirstLineRect(const QTextBlock& block) const;
    bool canInsertBlock() const;
    QWidget* m_gutter = nullptr;
    QToolButton* m_indicator = nullptr;
    ds::Theme m_themeOverride = ds::Theme::Count;
    bool m_gutterEnabled = false;
    bool m_composing = false;
    bool m_keyEditing = false, m_keyChanged = false;
    int m_typingEnd = -1, m_typingKind = 0;
    QElapsedTimer m_typingTimer;
};

} // namespace kl
