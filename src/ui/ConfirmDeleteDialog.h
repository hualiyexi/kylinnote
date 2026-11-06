#ifndef KYLINNOTE_CONFIRMDELETEDIALOG_H
#define KYLINNOTE_CONFIRMDELETEDIALOG_H

#include <QDialog>

class QCheckBox;

namespace kn {

// 首次删除时的确认框，带"不再提示"复选框
class ConfirmDeleteDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ConfirmDeleteDialog(QWidget *parent = nullptr);

    bool dontAskAgain() const;

private:
    QCheckBox *m_checkBox = nullptr;
};

} // namespace kn

#endif // KYLINNOTE_CONFIRMDELETEDIALOG_H
