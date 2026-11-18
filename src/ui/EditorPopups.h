#pragma once

#include <QDialog>
#include <QFrame>
#include <QImage>
#include <QPointer>
#include <QVector>

class QCompleter;
class QLineEdit;
class QListView;
class QPushButton;
class QStandardItemModel;
class QTextEdit;
class QTimer;

namespace kl {

struct EditorHint {
    QString value;
    QString text;
    QString toolTip;
    bool create = false;
};

// QCompleter provides native popup key forwarding without moving typing into a
// separate search field. Candidates are supplied by the editor's source context.
class EditorCompletion : public QObject {
    Q_OBJECT
public:
    explicit EditorCompletion(QTextEdit* editor, QObject* parent = nullptr);
    void showItems(const QVector<EditorHint>& items, const QRect& globalAnchor,
                   const QRect& globalBounds, const QString& accessibleName);
    void hide();
    bool isVisible() const;
    QWidget* popup() const;

signals:
    void chosen(QString value, bool create);
    void dismissed();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QPointer<QTextEdit> m_editor;
    QCompleter* m_completer = nullptr;
    QStandardItemModel* m_model = nullptr;
    QListView* m_view = nullptr;
};

// A child overlay, not an activating native popup: hovering a link must not steal
// the QTextEdit's focus. The two buttons remain reachable across the hover gap.
class LinkTools : public QFrame {
    Q_OBJECT
public:
    explicit LinkTools(QWidget* parent = nullptr);
    ~LinkTools() override;
    void showAt(const QRect& globalAnchor, const QRect& globalBounds);
    void scheduleHide();
    QRect cardGeometryGlobal() const;

signals:
    void editRequested();
    void copyRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void applyTheme();
    QTimer* m_hideTimer = nullptr;
    QPointer<QWidget> m_shadowLayer;
};

class LinkEditorPopup : public QDialog {
    Q_OBJECT
public:
    explicit LinkEditorPopup(QWidget* parent = nullptr);
    void openFor(const QString& label, const QString& target, bool existing,
                 const QRect& globalAnchor, const QRect& globalBounds);

signals:
    void submitted(QString label, QString target, bool remove);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void applyTheme();
    void validate();
    QWidget* m_card = nullptr;
    QLineEdit* m_text = nullptr;
    QLineEdit* m_target = nullptr;
    QPushButton* m_remove = nullptr;
    QPushButton* m_confirm = nullptr;
    QImage m_shadow;
};

} // namespace kl
