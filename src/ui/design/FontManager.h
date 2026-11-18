#pragma once

#include <QObject>
#include <QHash>
#include <QString>

class QApplication;
namespace kl {

// 稳定 ID 保存字体偏好，族名仅来自实际注册的随包静态字体。
class FontManager final : public QObject {
    Q_OBJECT
public:
    static FontManager& instance();
    QString preference() const { return m_preference; }
    QString family() const;
    QString familyFor(const QString& id) const;
    QStringList availableChoices() const;
    bool setPreference(const QString& id);
    void applyBootstrap(QApplication& app);

signals:
    void fontChanged(const QString& family);

private:
    FontManager();
    QString m_preference = QStringLiteral("default");
    mutable QHash<QString, QString> m_registeredFamilies;
};

} // namespace kl
