#pragma once
#include <QString>
class QApplication;
namespace kn {
namespace AppStyle {
void apply(QApplication& app);
QString previewCss();
}  // namespace AppStyle
}  // namespace kn
