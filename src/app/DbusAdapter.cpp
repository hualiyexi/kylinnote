#include "DbusAdapter.h"

#include "NoteManager.h"

#include <QDBusConnection>
#include <QDBusError>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDebug>

namespace kl {

namespace {
const char *kService   = "org.kylin.KylinList";
const char *kPath      = "/KylinList";
const char *kInterface = "org.kylin.KylinList";
}

const char *DbusAdapter::serviceName()
{
    return kService;
}

const char *DbusAdapter::objectPath()
{
    return kPath;
}

const char *DbusAdapter::interfaceName()
{
    return kInterface;
}

DbusAdapter::DbusAdapter(NoteManager *manager, QObject *parent)
    : QObject(parent),
      m_manager(manager)
{
}

bool DbusAdapter::registerService(QString *error)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        if (error) {
            *error = QStringLiteral("D-Bus \u4f1a\u8bdd\u603b\u7ebf\u4e0d\u53ef\u7528");
        }
        return false;
    }

    if (!bus.registerService(QLatin1String(kService))) {
        if (error) {
            *error = bus.lastError().message();
        }
        return false;
    }

    if (!bus.registerObject(QLatin1String(kPath), this,
                            QDBusConnection::ExportAllSlots)) {
        if (error) {
            *error = bus.lastError().message();
        }
        return false;
    }

    m_registered = true;
    return true;
}

bool DbusAdapter::notifyRunningInstance(QString *error)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        if (error) {
            *error = QStringLiteral("D-Bus \u4f1a\u8bdd\u603b\u7ebf\u4e0d\u53ef\u7528");
        }
        return false;
    }

    QDBusMessage call = QDBusMessage::createMethodCall(QLatin1String(kService),
                                                       QLatin1String(kPath),
                                                       QLatin1String(kInterface),
                                                       QLatin1String("showAll"));
    QDBusMessage reply = bus.call(call, QDBus::Block, 3000);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (error) {
            *error = reply.errorMessage();
        }
        return false;
    }
    return true;
}

void DbusAdapter::showAll()
{
    if (m_manager) {
        m_manager->showAll();
    }
}

void DbusAdapter::newNote()
{
    if (m_manager) {
        m_manager->createNote();
    }
}

} // namespace kl
