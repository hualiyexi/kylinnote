#ifndef KYLINLIST_DBUSADAPTER_H
#define KYLINLIST_DBUSADAPTER_H

#include <QObject>

namespace kl {

class NoteManager;

// 会话总线上的单实例 IPC：重复启动时把意图转交给已运行的实例
class DbusAdapter : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kylin.KylinList")

public:
    explicit DbusAdapter(NoteManager *manager, QObject *parent = nullptr);

    static const char *serviceName();
    static const char *objectPath();
    static const char *interfaceName();

    // 注册服务与对象；失败时调用方降级为"仅锁文件"模式
    bool registerService(QString *error = nullptr);

    // 通知已运行实例显示所有便签
    static bool notifyRunningInstance(QString *error = nullptr);

public slots:
    void showAll();
    void newNote();

private:
    NoteManager *m_manager = nullptr;
    bool m_registered = false;
};

} // namespace kl

#endif // KYLINLIST_DBUSADAPTER_H
