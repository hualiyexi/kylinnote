#ifndef KYLINLIST_DATEPICKERCONTROLS_H
#define KYLINLIST_DATEPICKERCONTROLS_H

#include <QPushButton>
#include <QString>

class QLineEdit;
class QSpinBox;
class QTime;
class QToolButton;

namespace kl {

class MenuComboBox;

namespace DatePickerUi {
constexpr int kSubCardWidth = 252;
constexpr int kRowHeight = 34;
constexpr int kControlHeight = 24;

QPushButton* actionButton(const QString& text, const char* objectName, QWidget* parent,
                          int height = kControlHeight);
MenuComboBox* comboBox(const QString& accessibleName, QWidget* parent);
QSpinBox* spinBox(int minimum, int maximum, const QString& accessibleName, QWidget* parent);
QLineEdit* fieldInput(const QString& placeholder, const QString& accessibleName, QWidget* parent);
bool parseTimeText(const QString& text, QTime* result);
QToolButton* toggleButton(const QString& text, QWidget* parent,
                          const QSize& size = QSize(28, kControlHeight));
void repolish(QWidget* widget);
void setInputInvalid(QLineEdit* edit, bool invalid, const QString& reason = QString());
} // namespace DatePickerUi

// 滴答式 34px 菜单行：整行可点击，选中态为 Primary 文字与右侧对勾，
// 不使用 QListWidgetItem 的左侧复选指示器。
class DatePickerMenuRow final : public QPushButton {
    Q_OBJECT
public:
    explicit DatePickerMenuRow(const QString& text, QWidget* parent = nullptr);

    void setSecondaryText(const QString& text);
    QString secondaryText() const { return m_secondaryText; }

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_secondaryText;
};

} // namespace kl

#endif // KYLINLIST_DATEPICKERCONTROLS_H
