#ifndef KYLINNOTE_COLORTHEME_H
#define KYLINNOTE_COLORTHEME_H

#include "../Common.h"

class QWidget;   // 注意：Qt 类的前向声明必须在全局作用域，不能落进 kn 命名空间

namespace kn {

// Windows 7 便笺风格的 6 色调色板，默认黄色
class ColorTheme
{
public:
    struct Spec {
        const char *key;        // 设置项/调试用的英文键
        const char *label;      // 中文名
        const char *body;       // 正文底色
        const char *bar;        // 标题条底色
        const char *border;     // 分隔线
        const char *text;       // 正文文字色
        const char *barText;    // 标题条文字色
        const char *selection;  // 选区底色
    };

    static Spec spec(NoteColor c);
    static NoteColor next(NoteColor c);
    static NoteColor fromInt(int v);
    static int toInt(NoteColor c);

    // 生成整窗样式表（NoteWidget / TitleBar / NoteEditor / StatusLabel / QToolButton）
    static QString windowStyle(NoteColor c);
    static void apply(QWidget *window, NoteColor c);
};

} // namespace kn

#endif // KYLINNOTE_COLORTHEME_H
