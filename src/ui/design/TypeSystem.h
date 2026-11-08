#pragma once
// 字体与字号系统：集中管理字体族、字号阶梯、字重、行高。
//
// 借鉴 CaseLawLibrary 的 --font-<scene> 场景变量与 useDetailReadingSize 的档位 bundle，
// 并补上前端缺失的全局字号阶梯 / 字重 / 行高 token（前端这些是散落的硬编码债）。
// 业务禁止 QFont("...") 裸构造，统一走 qfont(FontScene)。
#include <QFont>
#include <QString>

namespace kl {
namespace ds {

// 字号阶梯（pt）。考据主体 14px≈10.5pt、菜单 13px≈9.75pt、详情标题 20pt。
enum class TypeScale : int {
    Caption = 0,  // 辅助/计数
    Body,         // 正文/列表
    Subtitle,     // 次级标题/菜单
    Title,        // 面板标题
    Display,      // 大标题
    Count
};
qreal pointSize(TypeScale scale);

// 字体场景：镜像前端 --font-<scene>，含字重。
enum class FontScene : int {
    Base = 0,   // 默认 UI
    Body,       // 正文长文
    Display,    // 大标题
    Section,    // 分区小标题
    Caption,    // 辅助/计数
    Mono,       // 等宽/代码
    Count
};

// 集中导出：业务唯一取字体入口。
QFont qfont(FontScene scene);
// 字体族回退链（Microsoft YaHei UI → … → 系统默认）。
QStringList fontFamilies();

}  // namespace ds
}  // namespace kl
