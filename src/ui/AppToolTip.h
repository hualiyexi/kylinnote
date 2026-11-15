#ifndef KYLINLIST_APPTOOLTIP_H
#define KYLINLIST_APPTOOLTIP_H

#include <QPoint>
#include <QRect>
#include <QString>

class QApplication;
class QWidget;

namespace kl {
namespace AppToolTip {

// 应用内统一提示。系统托盘等原生平台提示不经过这里。
void install(QApplication& app);
void showText(const QPoint& globalPos, const QString& text,
              QWidget* context = nullptr, const QRect& target = QRect(),
              int msecShowTime = -1);
void hideText();
bool isVisible();
QWidget* window();

}  // namespace AppToolTip
}  // namespace kl

#endif  // KYLINLIST_APPTOOLTIP_H
