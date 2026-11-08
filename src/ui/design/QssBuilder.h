#pragma once
// QSS 生成器：Qt 5.12 QSS 没有变量，因此 QSS 是产物不是源。
// 模板用 %TOKEN% 占位符，运行时按当前主题替换为语义色字面量。
#include <QString>
#include "DesignTokens.h"

namespace kl {
namespace ds {

// 用指定主题渲染整份应用 QSS。
QString buildStyleSheet(Theme theme);
// 用指定主题渲染 Markdown 预览 CSS。
QString buildPreviewCss(Theme theme);

}  // namespace ds
}  // namespace kl
