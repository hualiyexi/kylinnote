#pragma once

#include <QColor>
#include <QIcon>
#include <QString>
#include "../domain/Entity.h"

class QPainter;
namespace kl {
namespace ListAppearance {

// icon 为空时使用语义默认图标；非空值既可以是内置图标 id，也可以是 emoji 文本。
QString iconName(const TaskList& list);
QColor color(const TaskList& list);
QIcon icon(const TaskList& list, int size, const QColor& fallbackColor);
void paint(QPainter* painter, const TaskList& list, const QRectF& rect,
           const QColor& fallbackColor);

}  // namespace ListAppearance
}  // namespace kl
