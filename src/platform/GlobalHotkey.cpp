#include "GlobalHotkey.h"

#include <QDebug>
#include <QKeySequence>
#include <QSocketNotifier>
#include <QTimer>

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>

namespace kn {

namespace {
// 抓取时必须穷举这些锁键组合，否则开了 NumLock 之后快捷键就失灵
const unsigned int kLockMasks[] = {
    0,
    LockMask,
    Mod2Mask,
    Mod5Mask,
    LockMask | Mod2Mask,
    LockMask | Mod5Mask,
    Mod2Mask | Mod5Mask,
    LockMask | Mod2Mask | Mod5Mask
};

unsigned int qtModsToX11(int qtMods)
{
    unsigned int x = 0;
    if (qtMods & Qt::ShiftModifier) {
        x |= ShiftMask;
    }
    if (qtMods & Qt::ControlModifier) {
        x |= ControlMask;
    }
    if (qtMods & Qt::AltModifier) {
        x |= Mod1Mask;
    }
    if (qtMods & Qt::MetaModifier) {
        x |= Mod4Mask;
    }
    return x;
}

// Xlib 默认错误处理器会打印后 exit(1)。抓键冲突相当常见
// （Ctrl+N 这类组合很多软件都在用），必须接管掉，否则程序会直接消失。
static int s_lastXError = 0;

int xErrorHandler(Display *, XErrorEvent *e)
{
    s_lastXError = e->error_code;
    return 0;
}

unsigned long qtKeyToKeysym(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return XK_a + (key - Qt::Key_A);
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return XK_0 + (key - Qt::Key_0);
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F35) {
        return XK_F1 + (key - Qt::Key_F1);
    }
    switch (key) {
    case Qt::Key_Escape:     return XK_Escape;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:    return XK_Tab;
    case Qt::Key_Backspace:  return XK_BackSpace;
    case Qt::Key_Return:
    case Qt::Key_Enter:      return XK_Return;
    case Qt::Key_Insert:     return XK_Insert;
    case Qt::Key_Delete:     return XK_Delete;
    case Qt::Key_Pause:      return XK_Pause;
    case Qt::Key_Print:      return XK_Print;
    case Qt::Key_Home:       return XK_Home;
    case Qt::Key_End:        return XK_End;
    case Qt::Key_Left:       return XK_Left;
    case Qt::Key_Up:         return XK_Up;
    case Qt::Key_Right:      return XK_Right;
    case Qt::Key_Down:       return XK_Down;
    case Qt::Key_PageUp:     return XK_Prior;
    case Qt::Key_PageDown:   return XK_Next;
    case Qt::Key_Space:      return XK_space;
    default:                 return 0;
    }
}
} // namespace

GlobalHotkey::GlobalHotkey(QObject *parent)
    : QObject(parent)
{
    m_dpy = XOpenDisplay(nullptr);
    if (!m_dpy) {
        qWarning() << "[kylinnote] 无法连接 X server，全局快捷键不可用";
        return;
    }

    const Window root = DefaultRootWindow(m_dpy);
    // 只需接收按键，根窗口上抓取的事件会送到本连接
    XSelectInput(m_dpy, root, KeyPressMask);

    m_notifier = new QSocketNotifier(ConnectionNumber(m_dpy),
                                     QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated,
            this, &GlobalHotkey::onX11Readable);

    // Xlib 会把事件先读进自己的队列，socket 电平可能不再保持可读，
    // 因此额外用一个低频定时器兜底排空队列，保证按键不会漏。
    QTimer *pump = new QTimer(this);
    pump->setInterval(100);
    connect(pump, &QTimer::timeout, this, &GlobalHotkey::onX11Readable);
    pump->start();
}

GlobalHotkey::~GlobalHotkey()
{
    if (m_dpy) {
        XCloseDisplay(m_dpy);
        m_dpy = nullptr;
    }
}

bool GlobalHotkey::grab(const QKeySequence &seq, int id)
{
    if (!m_dpy || seq.isEmpty()) {
        return false;
    }

    const int code = seq[0];
    const int qtKey = code & ~(int)Qt::KeyboardModifierMask;
    const int qtMods = code & (int)Qt::KeyboardModifierMask;

    const unsigned long keysym = qtKeyToKeysym(qtKey);
    if (keysym == 0) {
        qWarning() << "[kylinnote] 无法识别的快捷键:" << seq.toString();
        return false;
    }

    const KeyCode keycode = XKeysymToKeycode(m_dpy, (KeySym)keysym);
    if (keycode == 0) {
        qWarning() << "[kylinnote] 当前键盘布局无此按键:" << seq.toString();
        return false;
    }

    const unsigned int mods = qtModsToX11(qtMods);
    const Window root = DefaultRootWindow(m_dpy);

    XErrorHandler prev = XSetErrorHandler(xErrorHandler);
    s_lastXError = 0;
    unsigned int grabbed = 0;
    for (unsigned int lock : kLockMasks) {
        XGrabKey(m_dpy, keycode, mods | lock, root, True, GrabModeAsync, GrabModeAsync);
        XSync(m_dpy, False);
        if (s_lastXError != 0) {
            break;   // 第一次失败就停（通常是 BadAccess：已被占用）
        }
        ++grabbed;
    }
    XSetErrorHandler(prev);

    if (s_lastXError != 0) {
        // 抓取失败：把这一次已经抓上的组合全部撤回，避免半生效状态
        XErrorHandler guard = XSetErrorHandler(xErrorHandler);
        for (unsigned int i = 0; i < grabbed; ++i) {
            XUngrabKey(m_dpy, keycode, mods | kLockMasks[i], root);
        }
        XSync(m_dpy, False);
        XSetErrorHandler(guard);
        qWarning() << "[kylinnote] 快捷键" << seq.toString()
                   << "抓取失败（可能已被其它程序占用）";
        return false;
    }

    m_bindings.append(Binding{ (int)keycode, mods, id });
    return true;
}

void GlobalHotkey::clear()
{
    if (!m_dpy) {
        return;
    }
    const Window root = DefaultRootWindow(m_dpy);
    for (const Binding &b : m_bindings) {
        for (unsigned int lock : kLockMasks) {
            XUngrabKey(m_dpy, (KeyCode)b.keycode, b.mods | lock, root);
        }
    }
    m_bindings.clear();
}

void GlobalHotkey::onX11Readable()
{
    if (!m_dpy) {
        return;
    }
    while (XPending(m_dpy) > 0) {
        XEvent event;
        XNextEvent(m_dpy, &event);
        if (event.type != KeyPress) {
            continue;
        }
        XKeyEvent &key = event.xkey;
        // 只看真正按下的修饰键，锁键位已被穷举抓取覆盖
        const unsigned int mods = (unsigned int)key.state
                                  & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
        for (const Binding &b : m_bindings) {
            if (b.keycode == (int)key.keycode && b.mods == mods) {
                emit activated(b.id);
                break;
            }
        }
    }
}

} // namespace kn
