#ifndef KYLINNOTE_GLOBALHOTKEY_H
#define KYLINNOTE_GLOBALHOTKEY_H

#include <QList>
#include <QObject>

class QSocketNotifier;
class QKeySequence;

// Xlib 前向声明，避免把 X11 头文件污染到所有包含本文件的编译单元
typedef struct _XDisplay Display;

namespace kn {

// X11 全局热键：用 XGrabKey 在根窗口上抓取按键，焦点在别的软件里也能响应。
//
// 为什么不用 Qt::ApplicationShortcut：它只在"本应用有窗口处于激活态"时生效。
// 便笺是 Qt::Tool 窗口且完全可以没有任何便签，指望它做全局入口不可靠；
// 挂在不可见的宿主 widget 上则根本不会被注册，快捷键静默失效。
//
// XGrabKey 要求精确匹配 modifiers，而 NumLock/CapsLock/ScrollLock 会改变
// 事件里的 state，所以对每个锁键组合各抓一次（经典做法）。
class GlobalHotkey : public QObject
{
    Q_OBJECT
public:
    explicit GlobalHotkey(QObject *parent = nullptr);
    ~GlobalHotkey() override;

    // X server 是否可用
    bool isAvailable() const { return m_dpy != nullptr; }

    // 抓取一个快捷键；id 由调用方定义，触发时通过 activated(id) 带出。
    // 返回 false 表示该组合抓取失败（通常是已被其它程序占用）。
    bool grab(const QKeySequence &seq, int id);

    // 释放全部抓取
    void clear();

signals:
    void activated(int id);

private slots:
    void onX11Readable();

private:
    struct Binding {
        int          keycode = 0;
        unsigned int mods    = 0;
        int          id      = 0;
    };

    Display         *m_dpy      = nullptr;
    QSocketNotifier *m_notifier = nullptr;
    QList<Binding>   m_bindings;
};

} // namespace kn

#endif // KYLINNOTE_GLOBALHOTKEY_H
