#include "SystemShadow.h"

#include <QApplication>
#include <QEvent>
#include <QWidget>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace kl {
namespace {

#ifdef Q_OS_WIN
void clearDropShadow(WId id) {
    if (!id) return;
    HWND window = reinterpret_cast<HWND>(id);
    if (!IsWindow(window)) return;
    const LONG_PTR style = GetClassLongPtrW(window, GCL_STYLE);
    if (style & CS_DROPSHADOW)
        SetClassLongPtrW(window, GCL_STYLE, style & ~static_cast<LONG_PTR>(CS_DROPSHADOW));
}
#endif

class ShadowSuppressor final : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
#ifdef Q_OS_WIN
        if ((event->type() == QEvent::WinIdChange || event->type() == QEvent::Show)
            && watched->isWidgetType()) {
            auto* widget = static_cast<QWidget*>(watched);
            // internalWinId() 不会触发原生窗口创建；WinIdChange 时句柄已经存在。
            if (widget->isWindow()) clearDropShadow(widget->internalWinId());
        }
#else
        Q_UNUSED(watched);
        Q_UNUSED(event);
#endif
        return false;
    }
};

}  // namespace

void disableNativeWindowShadows() {
    static bool installed = false;
    if (installed || !qApp) return;
    installed = true;
    qApp->installEventFilter(new ShadowSuppressor(qApp));
}

}  // namespace kl
