#include "ConfirmDeleteDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace kn {

ConfirmDeleteDialog::ConfirmDeleteDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("KylinNote"));
    setModal(true);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setWindowFlags((windowFlags() & ~Qt::WindowContextHelpButtonHint)
                   | Qt::WindowStaysOnTopHint);   // 避免被置顶的便签遮挡

    QLabel *label = new QLabel(QString::fromUtf8("\u786e\u5b9a\u5220\u9664\u8fd9\u6761\u4fbf\u7b3a\u5417\uff1f"), this);

    QCheckBox *check = new QCheckBox(QString::fromUtf8("\u4e0d\u518d\u63d0\u793a"), this);
    m_checkBox = check;

    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QString::fromUtf8("\u5220\u9664"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QString::fromUtf8("\u53d6\u6d88"));

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);
    layout->addWidget(label);
    layout->addWidget(check);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    resize(320, 140);
}

bool ConfirmDeleteDialog::dontAskAgain() const
{
    return m_checkBox && m_checkBox->isChecked();
}

} // namespace kn
