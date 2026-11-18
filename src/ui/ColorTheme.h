#ifndef KYLINLIST_COLORTHEME_H
#define KYLINLIST_COLORTHEME_H

#include "../Common.h"
#include "design/DesignTokens.h"
class QWidget;

namespace kl {

class ColorTheme {
public:
    struct Spec {
        const char* key;
        const char* label;
        QString body, bar, border, text, barText, selection;
    };
    // Paper colors have their own fixed palette, independent of application appearance.
    static constexpr ds::Theme paperTheme() { return ds::Theme::Light; }
    static QColor borderColor();
    static Spec spec(NoteColor color);
    static NoteColor next(NoteColor color);
    static NoteColor fromInt(int value);
    static int toInt(NoteColor color);
    static QString windowStyle(NoteColor color);
    static void apply(QWidget* window, NoteColor color);
};

} // namespace kl

#endif // KYLINLIST_COLORTHEME_H
