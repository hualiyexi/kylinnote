#pragma once
#include <QWidget>
#include "../domain/Task.h"
class QTextEdit;
class QStackedWidget;
class QTabBar;
namespace kn {
class DocumentPreview;
class MarkdownEditor : public QWidget {
    Q_OBJECT
public:
    explicit MarkdownEditor(QWidget* parent = nullptr);
    void setContent(const QString& content, ContentFormat format);
    QString content() const;
    ContentFormat contentFormat() const {
        return m_format;
    }
    QTextEdit* textEditor() const {
        return m_editor;
    }
    void setPreview(bool preview);
signals:
    void contentChanged();

private:
    void applyInline(const QString& before, const QString& after, int formatKind);
    void applyBlock(const QString& prefix, int kind);
    void refreshPreview();
    void insertLink();
    QTextEdit* m_editor = nullptr;
    DocumentPreview* m_preview = nullptr;
    QStackedWidget* m_stack = nullptr;
    QTabBar* m_tabs = nullptr;
    ContentFormat m_format = ContentFormat::Markdown;
    bool m_loading = false;
};
}  // namespace kn
