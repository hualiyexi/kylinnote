#ifndef KYLINLIST_INSTANCECHANNEL_H
#define KYLINLIST_INSTANCECHANNEL_H

#include <QObject>
#include <QString>

class QLocalServer;
class QLocalSocket;

namespace kl {

// 跨平台单实例 IPC：基于 QtNetwork 的 QLocalServer / QLocalSocket
// （Windows 为命名管道，Linux/macOS 为 Unix domain socket）。
// 取代原先依赖 D-Bus 会话总线的唤醒通道，Windows 上无需 D-Bus。
//
// 典型用法：
//   主实例（抢到 SingleInstanceGuard 锁之后）：
//       auto *channel = new InstanceChannel(this);
//       connect(channel, &InstanceChannel::showAllRequested, ...);
//       channel->listen();   // 失败仅影响"二次启动唤醒"，可降级继续运行
//   次实例（抢锁失败）：
//       if (InstanceChannel::notifyRunningInstance(&err)) return 0;
//
// 协议：UTF-8 文本，一行一条消息（'\n' 分帧）。内置两条约定消息：
//   "show-all"  显示全部便签（二次启动唤醒）
//   "new-note"  在已运行实例中新建便签
class InstanceChannel : public QObject
{
    Q_OBJECT

public:
    explicit InstanceChannel(QObject *parent = nullptr);
    ~InstanceChannel() override;

    // 每用户唯一通道名（kylinlist-<user>.ipc），避免多用户共用机器时互相打扰
    static QString defaultServerName();

    // 成为主实例并开始监听。已有活实例在监听时返回 false；
    // 残留的死 socket（崩溃遗留）会被自动清理后重试一次。
    bool listen(QString *error = nullptr);
    void close();
    bool isListening() const;

    // 向主实例发送一条消息；连接失败（没有主实例）返回 false。
    static bool sendToPrimary(const QString &message, QString *error = nullptr,
                              int timeoutMs = 1500);

    // 二次启动唤醒的便捷封装：等价于 sendToPrimary(showAllMessage(), error)
    static bool notifyRunningInstance(QString *error = nullptr);

    static QString showAllMessage(); // "show-all"
    static QString newNoteMessage(); // "new-note"

signals:
    // 任意消息都会发出（已去掉行尾）；调用方可自行扩展消息类型
    void messageReceived(const QString &message);
    // 约定消息的定向信号，便于直接 connect
    void showAllRequested();
    void newNoteRequested();

private:
    void acceptPendingConnections();
    void readSocket(QLocalSocket *socket);
    void dispatchMessage(const QString &message);

    QLocalServer *m_server = nullptr;
};

} // namespace kl

#endif // KYLINLIST_INSTANCECHANNEL_H
