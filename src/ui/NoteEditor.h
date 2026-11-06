#ifndef KYLINNOTE_NOTEEDITOR_H
#define KYLINNOTE_NOTEEDITOR_H

#include <QTextEdit>

class QKeyEvent;
class QWheelEvent;

namespace kn {

// 便签正文编辑器：承担字号缩放、删除线、列表循环等编辑类快捷键
class NoteEditor : public QTextEdit
{
    Q_OBJECT
public:
    explicit NoteEditor(QWidget *parent = nullptr);

public slots:
    void increaseFontSize();
    void decreaseFontSize();
    void toggleStrikeThrough();
    void cycleListStyle();

signals:
    void fontSizeChanged(int pointSize);

protected:
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void applyFontSizeDelta(int delta);
    void removeListAtCursor();
    int currentFontSize() const;
};

} // namespace kn

#endif // KYLINNOTE_NOTEEDITOR_H
