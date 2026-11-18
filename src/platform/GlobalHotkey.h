#ifndef KYLINLIST_GLOBALHOTKEY_H
#define KYLINLIST_GLOBALHOTKEY_H

#include <QList>
#include <QObject>

class QSocketNotifier;
class QKeySequence;

// Xlib 前向声明，避免把 X11 头文件污染到所有包含本文件的编译单元
typedef struct _XDisplay Display;

namespace kl {

// Windows 实现的内部类，只在 GlobalHotkey.cpp 里定义
class WinHotkeyFilter;

// 全局热键：焦点在别的软件里也能响应。
//
// - X11（KYLINLIST_HAVE_X11）：XGrabKey 在根窗口上抓键；
// - Windows（Q_OS_WIN）：RegisterHotKey 注册，QAbstractNativeEventFilter
//   接收 WM_HOTKEY（注册挂到调用线程，主线程事件循环取到后经过滤器派发）；
// - 其它平台：空实现，grab() 恒返回 false。
//
// 为什么不用 Qt::ApplicationShortcut：它只在"本应用有窗口处于激活态"时生效。
// 便笺是 Qt::Tool 窗口且完全可以没有任何便签，指望它做全局入口不可靠；
// 挂在不可见的宿主 widget 上则根本不会被注册，快捷键静默失效。
//
// XGrabKey 要求精确匹配 modifiers，而 NumLock/CapsLock/ScrollLock 会改变
// 事件里的 state，所以对每个锁键组合各抓一次（经典做法）。
// Windows 的 RegisterHotKey 由系统处理锁键状态，无需穷举。
//
// 使用约束：构造 / grab / clear / 析构须在同一线程（GUI 线程）；
// activated 的槽里销毁本对象请用 deleteLater()。
class GlobalHotkey : public QObject
{
    Q_OBJECT
public:
    explicit GlobalHotkey(QObject *parent = nullptr);
    ~GlobalHotkey() override;

    // 平台后端是否可用（X11：已连上 X server；Windows：原生事件过滤器已安装）
    bool isAvailable() const;

    // 抓取一个快捷键；id 由调用方定义，触发时通过 activated(id) 带出。
    // 可多次调用注册多组热键，clear() 一次性全部释放。
    // 与 X11 实现一致，只取序列的第一个组合（QKeySequence 可含多个组合）。
    // 返回 false 表示该组合抓取失败（通常是已被其它程序占用）；
    // 失败时不会留下半注册状态。
    bool grab(const QKeySequence &seq, int id);

    // 释放全部抓取
    void clear();

signals:
    void activated(int id);

private slots:
    void onX11Readable();

private:
    friend class WinHotkeyFilter;
    bool onWinHotkey(int internalId);

    struct Binding {
        int          keycode  = 0;   // X11: KeyCode；Windows: VK 码
        unsigned int mods     = 0;   // X11: Shift/Control/Mod1/Mod4；Windows: MOD_*
        int          id       = 0;   // 调用方 id，activated(int) 带出
        int          nativeId = 0;   // Windows RegisterHotKey 的 id；X11 未用
    };

    Display         *m_dpy       = nullptr;   // 仅 X11
    QSocketNotifier *m_notifier  = nullptr;   // 仅 X11
    WinHotkeyFilter *m_winFilter = nullptr;   // 仅 Windows
    QList<Binding>   m_bindings;
};

} // namespace kl

#endif // KYLINLIST_GLOBALHOTKEY_H
