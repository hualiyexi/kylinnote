#include "GlobalHotkey.h"

#include <QDebug>
#include <QKeySequence>

#ifdef KYLINLIST_HAVE_X11
#include <QSocketNotifier>
#include <QTimer>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>

namespace kl {

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
        qWarning() << "[kylinlist] 无法连接 X server，全局快捷键不可用";
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

bool GlobalHotkey::isAvailable() const
{
    return m_dpy != nullptr;
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
        qWarning() << "[kylinlist] 无法识别的快捷键:" << seq.toString();
        return false;
    }

    const KeyCode keycode = XKeysymToKeycode(m_dpy, (KeySym)keysym);
    if (keycode == 0) {
        qWarning() << "[kylinlist] 当前键盘布局无此按键:" << seq.toString();
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
        qWarning() << "[kylinlist] 快捷键" << seq.toString()
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

bool GlobalHotkey::onWinHotkey(int)
{
    return false;
}

} // namespace kl
#elif defined(Q_OS_WIN)
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <qt_windows.h>

namespace kl {

namespace {

unsigned int qtModsToWin(int qtMods)
{
    unsigned int m = 0;
    if (qtMods & Qt::ShiftModifier) {
        m |= MOD_SHIFT;
    }
    if (qtMods & Qt::ControlModifier) {
        m |= MOD_CONTROL;
    }
    if (qtMods & Qt::AltModifier) {
        m |= MOD_ALT;
    }
    if (qtMods & Qt::MetaModifier) {
        m |= MOD_WIN;
    }
    return m;
}

// VK 码覆盖范围与 X11 版 qtKeyToKeysym 保持一致（F 键只到 VK_F24）
UINT qtKeyToVirtualKey(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return (UINT)('A' + (key - Qt::Key_A));
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return (UINT)('0' + (key - Qt::Key_0));
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        return VK_F1 + (UINT)(key - Qt::Key_F1);
    }
    switch (key) {
    case Qt::Key_Escape:     return VK_ESCAPE;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:    return VK_TAB;
    case Qt::Key_Backspace:  return VK_BACK;
    case Qt::Key_Return:
    case Qt::Key_Enter:      return VK_RETURN;
    case Qt::Key_Insert:     return VK_INSERT;
    case Qt::Key_Delete:     return VK_DELETE;
    case Qt::Key_Pause:      return VK_PAUSE;
    case Qt::Key_Print:      return VK_SNAPSHOT;
    case Qt::Key_Home:       return VK_HOME;
    case Qt::Key_End:        return VK_END;
    case Qt::Key_Left:       return VK_LEFT;
    case Qt::Key_Up:         return VK_UP;
    case Qt::Key_Right:      return VK_RIGHT;
    case Qt::Key_Down:       return VK_DOWN;
    case Qt::Key_PageUp:     return VK_PRIOR;
    case Qt::Key_PageDown:   return VK_NEXT;
    case Qt::Key_Space:      return VK_SPACE;
    default:                 return 0;
    }
}

// RegisterHotKey 的 id 在同一线程内必须唯一，文档要求取值 0x0000..0xBFFF。
// 用进程级递增计数器分配，避免多实例撞号；要撞上环回需要 4 万多次注册，
// 对"启动时注册几个热键"的用法不构成现实风险。
int nextWinHotkeyId()
{
    static int counter = 0;
    if (counter >= 0xBFFF) {
        counter = 1;
    } else {
        ++counter;
    }
    return counter;
}

} // namespace

// RegisterHotKey(NULL, ...) 把热键挂到调用线程的消息队列，系统在按键命中时
// 投递 WM_HOTKEY；Qt 事件循环（GUI 线程）取到后交给原生事件过滤器派发，
// 所以应用没有焦点也能收到——这正是全局热键的语义。
class WinHotkeyFilter : public QAbstractNativeEventFilter
{
public:
    explicit WinHotkeyFilter(GlobalHotkey *owner)
        : m_owner(owner) {}

    bool nativeEventFilter(const QByteArray &eventType, void *message, long *result) override
    {
        Q_UNUSED(result);
        if (eventType != QByteArrayLiteral("windows_generic_MSG")) {
            return false;
        }
        MSG *msg = static_cast<MSG *>(message);
        if (msg->message != WM_HOTKEY) {
            return false;
        }
        // 注册线程 == GUI 线程，与对象同线程，直接回调即可。
        // 回调之后不再访问任何成员：允许 activated 的槽里销毁 GlobalHotkey
        // （析构会连带 delete 本过滤器），返回字面量不触碰 this。
        return m_owner->onWinHotkey(static_cast<int>(msg->wParam));
    }

private:
    GlobalHotkey *m_owner;
};

GlobalHotkey::GlobalHotkey(QObject *parent)
    : QObject(parent)
{
    // 常规顺序是先有 QCoreApplication 再建本对象；若构造早于应用对象，
    // grab() 里会再补装一次，isAvailable() 在装好前如实返回 false。
    if (QCoreApplication *app = QCoreApplication::instance()) {
        m_winFilter = new WinHotkeyFilter(this);
        app->installNativeEventFilter(m_winFilter);
    }
}

GlobalHotkey::~GlobalHotkey()
{
    clear();
    if (m_winFilter) {
        // QCoreApplication 可能已析构（比如本对象作为其子对象被连锁销毁），
        // 此时事件循环已停，列表不会再被读取，跳过卸载即可。
        if (QCoreApplication *app = QCoreApplication::instance()) {
            app->removeNativeEventFilter(m_winFilter);
        }
        delete m_winFilter;
        m_winFilter = nullptr;
    }
}

bool GlobalHotkey::isAvailable() const
{
    return m_winFilter != nullptr;
}

bool GlobalHotkey::grab(const QKeySequence &seq, int id)
{
    if (seq.isEmpty()) {
        return false;
    }
    if (!m_winFilter) {
        // 兜底：构造时 QCoreApplication 还不存在的话，现在再装过滤器
        QCoreApplication *app = QCoreApplication::instance();
        if (!app) {
            qWarning() << "[kylinlist] QCoreApplication 尚未创建，全局快捷键不可用";
            return false;
        }
        m_winFilter = new WinHotkeyFilter(this);
        app->installNativeEventFilter(m_winFilter);
    }

    const int code = seq[0];
    const int qtKey = code & ~(int)Qt::KeyboardModifierMask;
    const int qtMods = code & (int)Qt::KeyboardModifierMask;

    const UINT vk = qtKeyToVirtualKey(qtKey);
    if (vk == 0) {
        qWarning() << "[kylinlist] 无法识别的快捷键:" << seq.toString();
        return false;
    }

    // MOD_NOREPEAT（Vista+）：按住不放不会连发
    const unsigned int mods = qtModsToWin(qtMods) | MOD_NOREPEAT;
    const int internalId = nextWinHotkeyId();

    // 一次 RegisterHotKey 即一次完整注册，失败时不会有半注册状态；
    // 组合已被任何程序（包括自己先前 grab 的）占用都会在这里失败
    // （1409 = ERROR_HOTKEY_ALREADY_REGISTERED）。GetLastError 必须在
    // 失败后立刻取值，否则可能被后续调用的参数求值冲掉。
    if (!RegisterHotKey(nullptr, internalId, mods, vk)) {
        const DWORD err = GetLastError();
        qWarning() << "[kylinlist] 快捷键" << seq.toString()
                   << "注册失败（可能已被其它程序占用），错误码" << (long)err;
        return false;
    }

    m_bindings.append(Binding{ (int)vk, mods, id, internalId });
    return true;
}

void GlobalHotkey::clear()
{
    // UnregisterHotKey 必须在注册线程调用；本类约定全程驻留 GUI 线程
    for (const Binding &b : m_bindings) {
        UnregisterHotKey(nullptr, b.nativeId);
    }
    m_bindings.clear();
}

bool GlobalHotkey::onWinHotkey(int internalId)
{
    for (const Binding &b : m_bindings) {
        if (b.nativeId == internalId) {
            emit activated(b.id);
            return true;
        }
    }
    return false;
}

void GlobalHotkey::onX11Readable()
{
    // 非 X11 平台不会被调用；给链接器一个符号即可
}

} // namespace kl
#else
namespace kl {
GlobalHotkey::GlobalHotkey(QObject *parent):QObject(parent) {}
GlobalHotkey::~GlobalHotkey()=default;
bool GlobalHotkey::isAvailable() const {return false;}
bool GlobalHotkey::grab(const QKeySequence &,int) {return false;}
void GlobalHotkey::clear() {}
void GlobalHotkey::onX11Readable() {}
bool GlobalHotkey::onWinHotkey(int) { return false; }
}
#endif
