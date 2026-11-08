#include "InstanceChannel.h"
#include "../Common.h"

#include <QCryptographicHash>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QVariant>

namespace kl {
namespace {

// 通道名只允许安全字符：Windows 上它是命名管道名，Unix 上是 socket 文件名
QString sanitizeNameComponent(const QString &raw)
{
    QString out;
    out.reserve(raw.size());
    for (const QChar &c : raw) {
        out.append((c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-'))
                       ? c
                       : QLatin1Char('-'));
    }
    return out;
}

QString currentUserName()
{
    QString user = QString::fromLocal8Bit(qgetenv("USER")); // Unix
    if (user.isEmpty()) {
        user = QString::fromLocal8Bit(qgetenv("USERNAME")); // Windows
    }
    if (user.isEmpty()) {
        user = QString::fromLocal8Bit(qgetenv("LOGNAME"));
    }
    return user;
}

} // namespace

InstanceChannel::InstanceChannel(QObject *parent)
    : QObject(parent)
{
}

InstanceChannel::~InstanceChannel()
{
    close();
}

QString InstanceChannel::defaultServerName()
{
    QString user = sanitizeNameComponent(currentUserName());
    if (user.isEmpty()) {
        user = QStringLiteral("default");
    }
    // Match the SingleInstanceGuard's data directory. A test/portable instance
    // with --data-dir must never wake an unrelated real-user instance.
    QString dataPath = QDir::cleanPath(QDir(dataDirPath()).absolutePath());
#ifdef Q_OS_WIN
    dataPath = dataPath.toCaseFolded();
#endif
    const QString suffix = QString::fromLatin1(QCryptographicHash::hash(
        dataPath.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
    return QStringLiteral("kylinlist-") + user.left(24) + QLatin1Char('-')
           + suffix + QStringLiteral(".ipc");
}

bool InstanceChannel::listen(QString *error)
{
    if (isListening()) {
        return true;
    }

    if (!m_server) {
        m_server = new QLocalServer(this);
        // 仅本人可连，防止同机其他用户伪造唤醒消息
        m_server->setSocketOptions(QLocalServer::UserAccessOption);
        connect(m_server, &QLocalServer::newConnection,
                this, &InstanceChannel::acceptPendingConnections);
    }

    const QString name = defaultServerName();
    if (m_server->listen(name)) {
        return true;
    }

    // listen 失败：区分"已有活实例"与"崩溃残留的死 socket"
    QLocalSocket probe;
    probe.connectToServer(name);
    if (probe.waitForConnected(300)) {
        if (error) {
            *error = QStringLiteral("已有实例正在监听 ") + name;
        }
        return false;
    }
    probe.abort();

    QLocalServer::removeServer(name); // 清理残留后重试一次
    if (m_server->listen(name)) {
        return true;
    }

    if (error) {
        *error = QStringLiteral("监听 ") + name + QStringLiteral(" 失败: ")
                 + m_server->errorString();
    }
    return false;
}

void InstanceChannel::close()
{
    if (m_server) {
        m_server->close(); // QLocalServer 关闭时会移除自己创建的 socket 文件
    }
}

bool InstanceChannel::isListening() const
{
    return m_server && m_server->isListening();
}

bool InstanceChannel::sendToPrimary(const QString &message, QString *error, int timeoutMs)
{
    QLocalSocket socket;
    socket.connectToServer(defaultServerName());
    if (!socket.waitForConnected(timeoutMs)) {
        if (error) {
            *error = QStringLiteral("连接主实例失败: ") + socket.errorString();
        }
        return false;
    }

    QByteArray payload = message.toUtf8();
    payload.replace('\n', ' '); // 协议按行分帧，单条消息不得含换行
    payload.append('\n');

    if (socket.write(payload) == -1 || !socket.waitForBytesWritten(timeoutMs)) {
        if (error) {
            *error = QStringLiteral("发送消息失败: ") + socket.errorString();
        }
        return false;
    }
    socket.flush();
    socket.disconnectFromServer();
    return true;
}

bool InstanceChannel::notifyRunningInstance(QString *error)
{
    return sendToPrimary(showAllMessage(), error);
}

QString InstanceChannel::showAllMessage()
{
    return QStringLiteral("show-all");
}

QString InstanceChannel::newNoteMessage()
{
    return QStringLiteral("new-note");
}

void InstanceChannel::acceptPendingConnections()
{
    if (!m_server) {
        return;
    }
    while (m_server->hasPendingConnections()) {
        QLocalSocket *socket = m_server->nextPendingConnection();
        if (!socket) {
            break;
        }
        // 用动态属性保存每个连接的半行缓冲，避免跨连接串帧
        socket->setProperty("rxBuffer", QByteArray());
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
            readSocket(socket);
        });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
            // 对端可能写完最后半行就直接断开，断开前再尽力读一次
            readSocket(socket);
            dispatchMessage(QString::fromUtf8(socket->property("rxBuffer").toByteArray()).trimmed());
            socket->deleteLater();
        });
        // 数据可能已先于 connect 到达
        if (socket->bytesAvailable() > 0) {
            readSocket(socket);
        }
    }
}

void InstanceChannel::readSocket(QLocalSocket *socket)
{
    if (!socket) {
        return;
    }
    QByteArray buffer = socket->property("rxBuffer").toByteArray();
    buffer.append(socket->readAll());

    int pos = -1;
    while ((pos = buffer.indexOf('\n')) >= 0) {
        const QByteArray line = buffer.left(pos);
        buffer.remove(0, pos + 1);
        dispatchMessage(QString::fromUtf8(line).trimmed());
    }
    socket->setProperty("rxBuffer", buffer);
}

void InstanceChannel::dispatchMessage(const QString &message)
{
    if (message.isEmpty()) {
        return;
    }
    emit messageReceived(message);
    if (message == showAllMessage()) {
        emit showAllRequested();
    } else if (message == newNoteMessage()) {
        emit newNoteRequested();
    }
}

} // namespace kl
