#pragma once

#include <QFrame>
#include "AppIcons.h"

class QLabel;
class QToolButton;
namespace kl {

// 侧栏可交互分区标题。按钮是真实 QWidget，不能用 delegate 画假按钮替代。
class SidebarSectionHeader : public QFrame {
    Q_OBJECT
public:
    explicit SidebarSectionHeader(const QString& title, const QString& objectName,
                                  QWidget* parent = nullptr);
    void setAction(IconId icon, const QString& toolTip, const QString& objectName,
                   bool collapseAction = false);
    void setLeadingIcon(IconId icon);
    void setExpanded(bool expanded);
    void setDisclosureEnabled(bool enabled);
    QToolButton* disclosureButton() const { return m_disclosure; }
    bool expanded() const { return m_expanded; }
    QToolButton* actionButton() const { return m_action; }

signals:
    void actionTriggered();
    void disclosureToggled();

protected:
    void enterEvent(QEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyTheme();
    void updateHover(bool hovered);
    QFrame* m_card = nullptr;
    QLabel* m_leadingIcon = nullptr;
    QLabel* m_title = nullptr;
    QToolButton* m_action = nullptr;
    QToolButton* m_disclosure = nullptr;
    bool m_disclosureEnabled = false;
    IconId m_leadingIconId = IconId::None;
    IconId m_actionIcon = IconId::None;
    bool m_collapseAction = false;
    bool m_expanded = true;
    bool m_hovered = false;
};

}  // namespace kl
