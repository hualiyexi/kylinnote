#pragma once

#include <QWidget>
#include "../app/Settings.h"
#include "AppIcons.h"

class QComboBox;
class QFrame;
class QLabel;
namespace kl {

class SmartListSettingsWidget : public QWidget {
    Q_OBJECT
public:
    explicit SmartListSettingsWidget(QWidget* parent = nullptr);

signals:
    void visibilityChanged();

private:
    struct Entry {
        QString key;
        QString label;
        IconId icon;
        QComboBox* combo = nullptr;
        QLabel* iconLabel = nullptr;
    };
    QFrame* buildSection(const QVector<Entry>& entries);
    void applyTheme();
    QVector<Entry> m_entries;
};

}  // namespace kl
